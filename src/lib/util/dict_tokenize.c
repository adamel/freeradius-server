/*
 *   This program is free software; you can redistribute it and/or modify
 *   it under the terms of the GNU General Public License as published by
 *   the Free Software Foundation; either version 2 of the License, or
 *   (at your option) any later version.
 *
 *   This program is distributed in the hope that it will be useful,
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *   GNU General Public License for more details.
 *
 *   You should have received a copy of the GNU General Public License
 *   along with this program; if not, write to the Free Software
 *   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA
 */

/** Parse dictionary files
 *
 * @file src/lib/util/dict_tokenize.c
 *
 * @copyright 2019 The FreeRADIUS server project
 */
RCSID("$Id$")

#include <freeradius-devel/util/dict_priv.h>
#include <freeradius-devel/util/syserror.h>
#include <freeradius-devel/util/rand.h>
#include <freeradius-devel/util/talloc.h>
#include <freeradius-devel/util/conf.h>
#include <freeradius-devel/radius/defs.h>

#include <sys/stat.h>
#include <ctype.h>

typedef struct dict_enum_fixup_s dict_enum_fixup_t;

#define MAX_ARGV (16)

/** A temporary enum value, which we'll resolve later
 *
 */
struct dict_enum_fixup_s {
	char			*filename;		//!< where the "enum" was defined
	int			line;			//!< ditto
	char			*attribute;		//!< we couldn't find (and will need to resolve later).
	char			*alias;			//!< Raw enum name.
	char			*value;			//!< Raw enum value.  We can't do anything with this until
							//!< we know the attribute type, which we only find out later.

	dict_enum_fixup_t	*next;			//!< Next in the linked list of fixups.
};
typedef struct dict_group_fixup_s dict_group_fixup_t;

/** A temporary group reference, which we'll resolve later
 *
 */
struct dict_group_fixup_s {
	char			*filename;		//!< where the "group" was defined
	int			line;			//!< ditto
	fr_dict_attr_t		*da;			//!< FR_TYPE_GROUP to fix
	char 			*ref;			//!< the reference name
	dict_group_fixup_t	*next;			//!< Next in the linked list of fixups.
};

/** Parser context for dict_from_file
 *
 * Allows vendor and TLV context to persist across $INCLUDEs
 */
#define MAX_STACK (32)
typedef struct {
	fr_dict_t		*dict;			//!< The dictionary before the current BEGIN-PROTOCOL block.
	char			*filename;		//!< name of the file we're reading
	int			line;			//!< line number of this file
	fr_dict_attr_t const	*da;			//!< the da we care about
	fr_type_t		nest;			//!< for manual vs automatic begin / end things
	int			member_num;		//!< structure member numbers
} dict_tokenize_frame_t;

typedef struct {
	fr_dict_t		*dict;			//!< Protocol dictionary we're inserting attributes into.

	dict_tokenize_frame_t	stack[MAX_STACK];     	//!< stack of attributes to track
	int			stack_depth;		//!< points to the last used stack frame

	fr_dict_attr_t const	*value_attr;		//!< Cache of last attribute to speed up
							///< value processing.
	fr_dict_attr_t const	*relative_attr;		//!< for ".82" instead of "1.2.3.82".
							///< only for parents of type "tlv"

	TALLOC_CTX		*fixup_pool;		//!< Temporary pool for fixups, reduces holes

	dict_enum_fixup_t	*enum_fixup;
	dict_group_fixup_t	*group_fixup;
} dict_tokenize_ctx_t;

/*
 *	String split routine.  Splits an input string IN PLACE
 *	into pieces, based on spaces.
 */
int fr_dict_str_to_argv(char *str, char **argv, int max_argc)
{
	int argc = 0;

	while (*str) {
		if (argc >= max_argc) break;

		/*
		 *	Chop out comments early.
		 */
		if (*str == '#') {
			*str = '\0';
			break;
		}

		while ((*str == ' ') ||
		       (*str == '\t') ||
		       (*str == '\r') ||
		       (*str == '\n'))
			*(str++) = '\0';

		if (!*str) break;

		argv[argc] = str;
		argc++;

		while (*str &&
		       (*str != ' ') &&
		       (*str != '\t') &&
		       (*str != '\r') &&
		       (*str != '\n'))
			str++;
	}

	return argc;
}

static int dict_read_sscanf_i(unsigned int *pvalue, char const *str)
{
	int rcode = 0;
	int base = 10;
	static char const *tab = "0123456789";

	if ((str[0] == '0') &&
	    ((str[1] == 'x') || (str[1] == 'X'))) {
		tab = "0123456789abcdef";
		base = 16;

		str += 2;
	}

	while (*str) {
		char const *c;

		if (*str == '.') break;

		c = memchr(tab, tolower((int)*str), base);
		if (!c) return 0;

		rcode *= base;
		rcode += (c - tab);
		str++;
	}

	*pvalue = rcode;
	return 1;
}

/** Set a new root dictionary attribute
 *
 * @note Must only be called once per dictionary.
 *
 * @param[in] dict		to modify.
 * @param[in] name		of dictionary root.
 * @param[in] proto_number	The artificial (or IANA allocated) number for the protocol.
 *				This is only used for
 * @return
 *	- 0 on success.
 *	- -1 on failure.
 */
static int dict_root_set(fr_dict_t *dict, char const *name, unsigned int proto_number)
{
	fr_dict_attr_flags_t flags = {
		.is_root = 1,
		.type_size = 1,
		.length = 1
	};

	if (!fr_cond_assert(!dict->root)) {
		fr_strerror_printf("Dictionary root already set");
		return -1;
	}

	dict->root = dict_attr_alloc_name(dict, name);
	if (!dict->root) return -1;

	dict_attr_init(dict->root, NULL, proto_number, FR_TYPE_TLV, &flags);
	DA_VERIFY(dict->root);

	return 0;
}

static int dict_process_type_field(char const *name, fr_type_t *type_p, fr_dict_attr_flags_t *flags)
{
	char *p;
	fr_type_t type;

	/*
	 *	Some types can have fixed length
	 */
	p = strchr(name, '[');
	if (p) *p = '\0';

	/*
	 *	find the type of the attribute.
	 */
	type = fr_table_value_by_str(fr_value_box_type_table, name, FR_TYPE_INVALID);
	if (type == FR_TYPE_INVALID) {
		fr_strerror_printf("Unknown data type '%s'", name);
		return -1;
	}

	if (p) {
		char *q;
		unsigned int length;

		if (type != FR_TYPE_OCTETS) {
			fr_strerror_printf("Only 'octets' types can have a 'length' parameter");
			return -1;
		}

		q = strchr(p + 1, ']');
		if (!q) {
			fr_strerror_printf("Invalid format for '%s[...]'", name);
			return -1;
		}

		*q = '\0';

		if (!dict_read_sscanf_i(&length, p + 1)) {
			fr_strerror_printf("Invalid length for '%s[...]'", name);
			return -1;
		}

		if ((length == 0) || (length > 253)) {
			fr_strerror_printf("Invalid length for '%s[...]'", name);
			return -1;
		}

		flags->length = length;
	}

	*type_p = type;
	return 0;
}


static int dict_process_flag_field(dict_tokenize_ctx_t *ctx, char *name, fr_type_t type, fr_dict_attr_flags_t *flags,
				   char **ref)
{
	char *p, *next = NULL;

	if (ref) *ref = NULL;

	for (p = name; p && *p != '\0' ; p = next) {
		char *key, *value;

		key = p;

		/*
		 *	Search for the first '=' or ','
		 */
		for (value = p + 1; *value && (*value != '=') && (*value != ','); value++) {
			/* do nothing */
		}

		/*
		 *	We have a value, zero out the '=' and point to the value.
		 */
		if (*value == '=') {
			*(value++) = '\0';
			if (!*value || (*value == ',')) {
				fr_strerror_printf("Missing value after '%s='", key);
				return -1;
			}
		}

		/*
		 *	Skip any trailing text in the value.
		 */
		for (next = value; *next; next++) {
			if (*next == ',') {
				*(next++) = '\0';
				break;
			}
		}

		/*
		 *	Boolean flag, means this is a tagged
		 *	attribute.
		 */
		if (strcmp(key, "has_tag") == 0) {
			if ((type != FR_TYPE_UINT32) && (type != FR_TYPE_STRING)) {
				fr_strerror_printf("The 'has_tag' flag can only be used for attributes of type 'integer' "
						   "or 'string'");
				return -1;
			}

			flags->has_tag = 1;

			/*
			 *	Encryption method.
			 */
		} else if (strcmp(key, "encrypt") == 0) {
			char *qq;

			flags->encrypt = strtol(value, &qq, 0);
			if (*qq) {
				fr_strerror_printf("Invalid encrypt value \"%s\"", value);
				return -1;
			}

			/*
			 *	Marks the attribute up as internal.
			 *	This means it can use numbers outside of the allowed
			 *	protocol range, and also means it will not be included
			 *	in replies or proxy requests.
			 */
		} else if (strcmp(key, "internal") == 0) {
			flags->internal = 1;

		} else if (strcmp(key, "array") == 0) {
			flags->array = 1;

		} else if (strcmp(key, "concat") == 0) {
			if (type != FR_TYPE_OCTETS) {
				fr_strerror_printf("The 'concat' flag can only be used for attributes of type 'octets'");
				return -1;
			}

			flags->concat = 1;

		} else if (strcmp(key, "virtual") == 0) {
			flags->virtual = 1;

		} else if (strcmp(key, "long") == 0) {
			if (type != FR_TYPE_EXTENDED) {
				fr_strerror_printf("The 'long' flag can only be used for attributes of type 'extended'");
				return -1;
			}
			flags->extra = 1;

		} else if (strcmp(key, "key") == 0) {
			if ((type != FR_TYPE_UINT8) && (type != FR_TYPE_UINT16) && (type != FR_TYPE_UINT32)) {
				fr_strerror_printf("The 'key' flag can only be used for attributes of type 'uint8', 'uint16', or 'uint32'");
				return -1;
			}

			flags->extra = 1;

		} else if (type == FR_TYPE_DATE) {
			flags->length = 4;
			flags->type_size = FR_TIME_RES_SEC;

			if (strncmp(key, "uint", 4) == 0) {
				fr_type_t subtype;

				subtype = fr_table_value_by_str(fr_value_box_type_table, name, FR_TYPE_INVALID);
				if (subtype == FR_TYPE_INVALID) {
				unknown_type:
					fr_strerror_printf("Unknown or unsupported date type '%s'", key);
					return -1;
				}

				switch (subtype) {
					default:
						goto unknown_type;

				case FR_TYPE_UINT16:
					flags->length = 2;
					break;

				case FR_TYPE_UINT32:
					flags->length = 4;
					break;

				case FR_TYPE_UINT64:
					flags->length = 8;
					break;
				}
			} else {
				int precision;

				precision = fr_table_value_by_str(date_precision_table, key, -1);
				if (precision < 0) {
					fr_strerror_printf("Unknown date precision '%s'", key);
					return -1;
				}
				flags->type_size = precision;
			}

		} else if (strcmp(key, "ref") == 0) {
			if (!ref || (type != FR_TYPE_GROUP)) {
				fr_strerror_printf("The 'ref' flag can only be used for attributes of type 'group'");
				return -1;
			}

			*ref = talloc_strdup(ctx->dict->pool, value);

		} else {
			fr_strerror_printf("Unknown option '%s'", key);
			return -1;
		}
	}

	/*
	 *	Check that the flags are valid.
	 */
	if (!dict_attr_flags_valid(ctx->dict, ctx->stack[ctx->stack_depth].da, name, NULL, type, flags)) return -1;

	return 0;
}


static int dict_ctx_push(dict_tokenize_ctx_t *ctx, fr_dict_attr_t const *da)
{
	if ((ctx->stack_depth + 1) >= MAX_STACK) {
		fr_strerror_printf_push("Attribute definitions are nested too deep.");
		return -1;
	}

	ctx->stack_depth++;
	memset(&ctx->stack[ctx->stack_depth], 0, sizeof(ctx->stack[ctx->stack_depth]));

	ctx->stack[ctx->stack_depth].dict = ctx->stack[ctx->stack_depth - 1].dict;
	ctx->stack[ctx->stack_depth].da = da;
	ctx->stack[ctx->stack_depth].filename = ctx->stack[ctx->stack_depth - 1].filename;
	ctx->stack[ctx->stack_depth].line = ctx->stack[ctx->stack_depth - 1].line;

	return 0;
}

static fr_dict_attr_t const *dict_ctx_unwind(dict_tokenize_ctx_t *ctx)
{
	while ((ctx->stack_depth > 0) &&
	       (ctx->stack[ctx->stack_depth].nest == FR_TYPE_INVALID)) {
		ctx->stack_depth--;
	}

	return ctx->stack[ctx->stack_depth].da;
}

/*
 *	Process the ATTRIBUTE command
 */
static int dict_read_process_attribute(dict_tokenize_ctx_t *ctx, char **argv, int argc,
				       fr_dict_attr_flags_t *base_flags)
{
	bool			set_relative_attr = true;

	ssize_t			slen;
	unsigned int		attr;

	fr_type_t      		type;
	fr_dict_attr_flags_t	flags;
	fr_dict_attr_t const	*parent;
	char			*ref = NULL;

	if ((argc < 3) || (argc > 4)) {
		fr_strerror_printf("Invalid ATTRIBUTE syntax");
		return -1;
	}

	/*
	 *	Dictionaries need to have real names, not shitty ones.
	 */
	if (strncmp(argv[0], "Attr-", 5) == 0) {
		fr_strerror_printf("Invalid ATTRIBUTE name");
		return -1;
	}

	memcpy(&flags, base_flags, sizeof(flags));

	if (dict_process_type_field(argv[2], &type, &flags) < 0) return -1;

	/*
	 *	Relative OIDs apply ONLY to attributes of type 'tlv'.
	 */
	if (type != FR_TYPE_TLV) set_relative_attr = false;

	/*
	 *	A non-relative ATTRIBUTE definition means that it is
	 *	in the context of the previous BEGIN-FOO.  So we
	 *	unwind the stack to match.
	 */
	if (argv[1][0] != '.') {
		parent = dict_ctx_unwind(ctx);

		/*
		 *	Allow '0xff00' as attribute numbers, but only
		 *	if there is no OID component.
		 */
		if (strchr(argv[1], '.') == 0) {
			if (!dict_read_sscanf_i(&attr, argv[1])) {
				fr_strerror_printf("Invalid ATTRIBUTE number");
				return -1;
			}

		} else {
			slen = fr_dict_attr_by_oid(ctx->dict, &parent, &attr, argv[1]);
			if (slen <= 0) return -1;
		}

	} else {
		if (!ctx->relative_attr) {
			fr_strerror_printf("Unknown parent for partial OID");
			return -1;
		}

		parent = ctx->relative_attr;
		set_relative_attr = false;

		slen = fr_dict_attr_by_oid(ctx->dict, &parent, &attr, argv[1]);
		if (slen <= 0) return -1;
	}

	if (!fr_cond_assert(parent)) return -1;	/* Should have provided us with a parent */

	/*
	 *	Members of a 'struct' MUST use MEMBER, not ATTRIBUTE.
	 */
	if (parent->type == FR_TYPE_STRUCT) {
		fr_strerror_printf("Member %s of ATTRIBUTE %s type 'struct' MUST use the \"MEMBER\" keyword",
				   argv[0], parent->name);
		return -1;
	}

	/*
	 *	Parse options.
	 */
	if ((argc >= 4) && (dict_process_flag_field(ctx, argv[3], type, &flags, &ref) < 0)) return -1;

#ifdef WITH_DICTIONARY_WARNINGS
	/*
	 *	Hack to help us discover which vendors have illegal
	 *	attributes.
	 */
	if (!vendor && (attr < 256) &&
	    !strstr(fn, "rfc") && !strstr(fn, "illegal")) {
		fprintf(stderr, "WARNING: Illegal Attribute %s in %s\n",
			argv[0], fn);
	}
#endif

#ifdef __clang_analyzer__
	if (!ctx->dict) return -1;
#endif

	/*
	 *	Add in an attribute
	 */
	if (fr_dict_attr_add(ctx->dict, parent, argv[0], attr, type, &flags) < 0) return -1;

	/*
	 *	Hack up groups according to "ref"
	 */
	if (type == FR_TYPE_GROUP) {
		fr_dict_attr_t const *da;
		fr_dict_attr_t *self;
		fr_dict_t const *dict;
		char *p;

		da = fr_dict_attr_child_by_num(parent, attr);
		if (!da) {
			fr_strerror_printf("Failed to find attribute '%s' we just added.", argv[0]);
			return -1;
		}

		memcpy(&self, &da, sizeof(self)); /* const issues */

		/*
		 *	No qualifiers, just point it to the root of the current dictionary.
		 */
		if (!ref) {
			dict = ctx->dict;
			da = ctx->dict->root;
			goto check;
		}

		da = fr_dict_attr_by_name(ctx->dict, ref);
		if (da) {
			dict = ctx->dict;
			goto check;
		}

		/*
		 *	The attribute doesn't exist, and the reference
		 *	is FOO, it might be just a ref to a
		 *	dictionary.
		 */
		p = strchr(ref, '.');
		if (!p) goto save;

		/*
		 *	Get / skip protocol name.
		 */
		slen = fr_dict_by_protocol_substr(&dict, ref, ctx->dict);
		if (slen < 0) {
			talloc_free(ref);
			return -1;
		}

		/*
		 *	No known dictionary, so we're asked to just
		 *	use the whole string.  Which we did above.  So
		 *	either it's a bad ref, OR it's a ref to a
		 *	dictionary which doesn't exist.
		 */
		if (slen == 0) {
			dict_group_fixup_t *fixup;

		save:
			fixup = talloc_zero(ctx->fixup_pool, dict_group_fixup_t);
			if (!fixup) {
			oom:
				talloc_free(ref);
				return -1;
			}

			fixup->filename = talloc_strdup(fixup, ctx->stack[ctx->stack_depth].filename);
			if (!fixup->filename) goto oom;
			fixup->line = ctx->stack[ctx->stack_depth].line;

			fixup->da = self;
			fixup->ref = ref;

			/*
			 *	Insert to the head of the list.
			 */
			fixup->next = ctx->group_fixup;
			ctx->group_fixup = fixup;

		} else if (ref[slen] == '\0') {
			da = dict->root;
			goto check;

		} else {
			/*
			 *	Look up the attribute.
			 */
			da = fr_dict_attr_by_name(dict, ref + slen);
			if (!da) {
				fr_strerror_printf("protocol loaded, but no attribute '%s'", ref + slen);
				talloc_free(ref);
				return -1;
			}

		check:
			if (da->type != FR_TYPE_TLV) {
				fr_strerror_printf("References MUST be to attributes of type 'tlv'");
				talloc_free(ref);
				return -1;
			}

			talloc_free(ref);
			self->dict = dict;
			self->ref = da;
		}
	}

	/*
	 *	If we need to set the previous attribute, we have to
	 *	look it up by number.  This lets us set the
	 *	*canonical* previous attribute, and not any potential
	 *	duplicate which was just added.
	 */
	if (set_relative_attr) ctx->relative_attr = fr_dict_attr_child_by_num(parent, attr);

	/*
	 *	Adding an attribute of type 'struct' is an implicit
	 *	BEGIN-STRUCT.
	 */
	if (type == FR_TYPE_STRUCT) {
		fr_dict_attr_t const *da = fr_dict_attr_child_by_num(parent, attr);

		if (!da) {
			fr_strerror_printf("Failed adding attribute %s", argv[0]);
			return -1;
		}

		if (dict_ctx_push(ctx, da) < 0) return -1;
	}

	return 0;
}


/*
 *	Process the MEMBER command
 */
static int dict_read_process_member(dict_tokenize_ctx_t *ctx, char **argv, int argc,
				       fr_dict_attr_flags_t *base_flags)
{
	fr_type_t      		type;
	fr_dict_attr_flags_t	flags;

	if ((argc < 2) || (argc > 3)) {
		fr_strerror_printf("Invalid MEMBER syntax");
		return -1;
	}

	if (ctx->stack[ctx->stack_depth].da->type != FR_TYPE_STRUCT) {
		fr_strerror_printf("MEMBER can only be used for ATTRIBUTEs of type 'struct'");
		return -1;
	}

	/*
	 *	Dictionaries need to have real names, not shitty ones.
	 */
	if (strncmp(argv[0], "Attr-", 5) == 0) {
		fr_strerror_printf("Invalid MEMBER name");
		return -1;
	}

	memcpy(&flags, base_flags, sizeof(flags));

	if (dict_process_type_field(argv[1], &type, &flags) < 0) return -1;

	/*
	 *	Parse options.
	 */
	if ((argc >= 3) && (dict_process_flag_field(ctx, argv[2], type, &flags, NULL) < 0)) return -1;

#ifdef __clang_analyzer__
	if (!ctx->dict) return -1;
#endif

	/*
	 *	Add the MEMBER to the parent.
	 */
	if (fr_dict_attr_add(ctx->dict, ctx->stack[ctx->stack_depth].da, argv[0], ++ctx->stack[ctx->stack_depth].member_num, type, &flags) < 0) return -1;

	/*
	 *	A 'struct' can have a MEMBER of type 'tlv', but ONLY
	 *	as the last entry in the 'struct'.  If we see that,
	 *	set the previous attribute to the TLV we just added.
	 *	This allows the children of the TLV to be parsed as
	 *	partial OIDs, so we don't need to know the full path
	 *	to them.
	 */
	if (type == FR_TYPE_TLV) {
		ctx->relative_attr = fr_dict_attr_child_by_num(ctx->stack[ctx->stack_depth].da, ctx->stack[ctx->stack_depth].member_num);
		if (dict_ctx_push(ctx, ctx->relative_attr) < 0) return -1;

	} else {
		fr_dict_attr_t *mutable;

		/*
		 *	Add the size of this member to the parent struct.
		 *
		 *	Note that we top out at 256.  This value is
		 *	informative, so we don't really care to know the exact
		 *	size.  Plus, most of the time it's less than 256.
		 */
		memcpy(&mutable, &ctx->stack[ctx->stack_depth].da, sizeof(mutable));
		if ( ((int) mutable->flags.length + flags.length) >= 256) {
			mutable->flags.length = 255;
		} else {
			mutable->flags.length += flags.length;
		}
	}

	return 0;
}


/** Process a value alias
 *
 */
static int dict_read_process_value(dict_tokenize_ctx_t *ctx, char **argv, int argc)
{
	fr_dict_attr_t const		*da;
	fr_value_box_t			value;

	if (argc != 3) {
		fr_strerror_printf("Invalid VALUE syntax");
		return -1;
	}

	/*
	 *	Most VALUEs are bunched together by ATTRIBUTE.  We can
	 *	save a lot of lookups on dictionary initialization by
	 *	caching the last attribute for a VALUE.
	 */
	if (!ctx->value_attr || (strcasecmp(argv[0], ctx->value_attr->name) != 0)) {
		ctx->value_attr = fr_dict_attr_by_name(ctx->dict, argv[0]);
	}
	da = ctx->value_attr;

	/*
	 *	Remember which attribute is associated with this
	 *	value.  This allows us to define enum
	 *	values before the attribute exists, and fix them
	 *	up later.
	 */
	if (!da) {
		dict_enum_fixup_t *fixup;

		if (!fr_cond_assert_msg(ctx->fixup_pool, "fixup pool context invalid")) return -1;

		fixup = talloc_zero(ctx->fixup_pool, dict_enum_fixup_t);
		if (!fixup) {
		oom:
			talloc_free(fixup);
			fr_strerror_printf("Out of memory");
			return -1;
		}

		fixup->filename = talloc_strdup(fixup, ctx->stack[ctx->stack_depth].filename);
		if (!fixup->filename) goto oom;
		fixup->line = ctx->stack[ctx->stack_depth].line;

		fixup->attribute = talloc_strdup(fixup, argv[0]);
		if (!fixup->attribute) goto oom;
		fixup->alias = talloc_strdup(fixup, argv[1]);
		if (!fixup->alias) goto oom;
		fixup->value = talloc_strdup(fixup, argv[2]);
		if (!fixup->value) goto oom;

		/*
		 *	Insert to the head of the list.
		 */
		fixup->next = ctx->enum_fixup;
		ctx->enum_fixup = fixup;

		return 0;
	}

	/*
	 *	Only a few data types can have VALUEs defined.
	 */
	switch (da->type) {
	case FR_TYPE_ABINARY:
	case FR_TYPE_GROUP:
	case FR_TYPE_STRUCTURAL:
	case FR_TYPE_INVALID:
	case FR_TYPE_MAX:
		fr_strerror_printf_push("Cannot define VALUE for ATTRIBUTE \"%s\" of data type \"%s\"", da->name,
					fr_table_str_by_value(fr_value_box_type_table, da->type, "<INVALID>"));
		return -1;

	default:
		break;
	}

	{
		fr_type_t type = da->type;	/* Might change - Stupid combo IP */

		if (fr_value_box_from_str(NULL, &value, &type, NULL, argv[2], -1, '\0', false) < 0) {
			fr_strerror_printf_push("Invalid VALUE for ATTRIBUTE \"%s\"", da->name);
			return -1;
		}
	}

	if (fr_dict_enum_add_alias(da, argv[1], &value, false, true) < 0) {
		fr_value_box_clear(&value);
		return -1;
	}
	fr_value_box_clear(&value);

	return 0;
}

/*
 *	Process the FLAGS command
 */
static int dict_read_process_flags(UNUSED fr_dict_t *dict, char **argv, int argc,
				   fr_dict_attr_flags_t *base_flags)
{
	bool sense = true;

	if (argc == 1) {
		char *p;

		p = argv[0];
		if (*p == '!') {
			sense = false;
			p++;
		}

		if (strcmp(p, "internal") == 0) {
			base_flags->internal = sense;
			return 0;
		}
	}

	fr_strerror_printf("Invalid FLAGS syntax");
	return -1;
}


/** Process a STRUCT
 *
 *  Which MUST be a sub-structure of another struct
 */
static int dict_read_process_struct(dict_tokenize_ctx_t *ctx, char **argv, int argc)
{
	fr_dict_attr_t			*da, *mutable;
	fr_dict_attr_t const		*parent, *previous;
	fr_value_box_t			value;
	fr_type_t			type;
	unsigned int			attr;
	fr_dict_attr_flags_t		flags;

	if (argc != 3) {
		fr_strerror_printf("Invalid STRUCT syntax");
		return -1;
	}

	/*
	 *	This SHOULD be the "key" field.
	 */
	parent = fr_dict_attr_by_name(ctx->dict, argv[0]);
	if (!parent) {
		fr_strerror_printf("Unknown attribute '%s'", argv[0]);
		return -1;
	}

	if (!parent->flags.extra) {
		fr_strerror_printf("Attribute '%s' is not a 'key' attribute", argv[0]);
		return -1;
	}

	/*
	 *	Rely on dict_attr_flags_valid() to ensure that
	 *	da->type is an unsigned integer, AND that da->parent->type == struct
	 */
	if (!fr_cond_assert(parent->parent->type == FR_TYPE_STRUCT)) return -1;

	/*
	 *	The attribute in the current stack frame is NOT the
	 *	enclosing "struct": unwind until we do find the parent.
	 */
	if (ctx->stack[ctx->stack_depth].da != parent->parent) {
		int i;

		for (i = ctx->stack_depth - 1; i > 0; i--) {
			if (ctx->stack[i].da == parent->parent) {
				ctx->stack_depth = i;
				break;
			}
		}
	}

	/*
	 *	The attribute in the current stack frame MUST be the
	 *	enclosing "struct".
	 */
	if (ctx->stack[ctx->stack_depth].da != parent->parent) {
		fr_strerror_printf("Attribute '%s' is not a MEMBER of the current 'struct'", argv[0]);
		return -1;
	}

	/*
	 *	Check that the previous attribute exists, and is NOT a
	 *	variable-length type.
	 */
	previous = fr_dict_attr_child_by_num(ctx->stack[ctx->stack_depth].da, ctx->stack[ctx->stack_depth].member_num);
	if (!previous) return -1;	/* should never happen */

	switch (previous->type) {
	case FR_TYPE_FIXED_SIZE:
		break;

		/*
		 *	Fixed-size "octets" types are OK.
		 */
	case FR_TYPE_OCTETS:
		if (previous->flags.length != 0) break;
		/* FALL-THROUGH */

	default:
		fr_strerror_printf("STRUCT attribute '%s' cannot follow a variable-sized previous MEMBER attribute '%s'",
				   argv[0], previous->name);
		return -1;
	}

	/*
	 *	A STRUCT also defines a VALUE
	 */
	if (dict_read_process_value(ctx, argv, argc) < 0) return -1;

	/*
	 *	Re-parse the value, because it's simpler than doing a lookup.
	 */
	type = parent->type;	/* because of combo-IP nonsense */
	if (fr_value_box_from_str(NULL, &value, &type, NULL, argv[2], -1, '\0', false) < 0) {
		fr_strerror_printf_push("Invalid value for STRUCT \"%s\"", argv[2]);
		return -1;
	}

	switch (type) {
	case FR_TYPE_UINT8:
		attr = value.vb_uint8;
		break;

	case FR_TYPE_UINT16:
		attr = value.vb_uint16;
		break;

	case FR_TYPE_UINT32:
		attr = value.vb_uint32;
		break;

	default:
		fr_strerror_printf("Invalid data type in attribute '%s'", argv[0]);
		fr_value_box_clear(&value);
		return -1;
	}
	fr_value_box_clear(&value);

	memset(&flags, 0, sizeof(flags));
	da = dict_attr_alloc(ctx->dict->pool, parent, argv[1], attr, FR_TYPE_STRUCT, &flags);
	if (!da) {
		fr_strerror_printf("Failed allocating memory for STRUCT");
		return -1;
	}

	memcpy(&mutable, &parent, sizeof(parent)); /* const issues */
	if (dict_attr_child_add(mutable, da) < 0) {
		talloc_free(da);
		return -1;
	}

	/*
	 *	A STRUCT definition is an implicit BEGIN-STRUCT.
	 */
	ctx->relative_attr = NULL;
	if (dict_ctx_push(ctx, da) < 0) return -1;

	return 0;
}


static int dict_read_parse_format(char const *format, unsigned int *pvalue, int *ptype, int *plength,
				  bool *pcontinuation)
{
	char const *p;
	int type, length;
	bool continuation = false;

	if (strncasecmp(format, "format=", 7) != 0) {
		fr_strerror_printf("Invalid format for VENDOR.  Expected 'format=', got '%s'",
				   format);
		return -1;
	}

	p = format + 7;
	if ((strlen(p) < 3) ||
	    !isdigit((int)p[0]) ||
	    (p[1] != ',') ||
	    !isdigit((int)p[2]) ||
	    (p[3] && (p[3] != ','))) {
		fr_strerror_printf("Invalid format for VENDOR.  Expected text like '1,1', got '%s'",
				   p);
		return -1;
	}

	type = (int)(p[0] - '0');
	length = (int)(p[2] - '0');

	if ((type != 1) && (type != 2) && (type != 4)) {
		fr_strerror_printf("Invalid type value %d for VENDOR", type);
		return -1;
	}

	if ((length != 0) && (length != 1) && (length != 2)) {
		fr_strerror_printf("Invalid length value %d for VENDOR", length);
		return -1;
	}

	if (p[3] == ',') {
		if (!p[4]) {
			fr_strerror_printf("Invalid format for VENDOR.  Expected text like '1,1', got '%s'",
					   p);
			return -1;
		}

		if ((p[4] != 'c') ||
		    (p[5] != '\0')) {
			fr_strerror_printf("Invalid format for VENDOR.  Expected text like '1,1', got '%s'",
					   p);
			return -1;
		}
		continuation = true;

		if ((*pvalue != VENDORPEC_WIMAX) ||
		    (type != 1) || (length != 1)) {
			fr_strerror_printf("Only WiMAX VSAs can have continuations");
			return -1;
		}
	}

	*ptype = type;
	*plength = length;
	*pcontinuation = continuation;
	return 0;
}

/** Register the specified dictionary as a protocol dictionary
 *
 * Allows vendor and TLV context to persist across $INCLUDEs
 */
static int dict_read_process_protocol(char **argv, int argc)
{
	unsigned int	value;
	unsigned int	type_size = 1;
	fr_dict_t	*dict;
	fr_dict_attr_t	*mutable;

	if ((argc < 2) || (argc > 3)) {
		fr_strerror_printf("Missing arguments after PROTOCOL.  Expected PROTOCOL <num> <name>");
		return -1;
	}

	/*
	 *	 Validate all entries
	 */
	if (!dict_read_sscanf_i(&value, argv[1])) {
		fr_strerror_printf("Invalid number '%s' following PROTOCOL", argv[1]);
		return -1;
	}

	/*
	 *	255 protocolss FR_TYPE_GROUP type_size hack
	 */
	if ((value == 0) || (value > 255)) {
		fr_strerror_printf("Invalid value '%u' following PROTOCOL", value);
		return -1;
	}

	/*
	 *	Look for a format statement.  This may specify the
	 *	type length of the protocol's types.
	 */
	if (argc == 3) {
		char const *p;
		char *q;

		if (strncasecmp(argv[2], "format=", 7) != 0) {
			fr_strerror_printf("Invalid format for PROTOCOL.  Expected 'format=', got '%s'", argv[2]);
			return -1;
		}
		p = argv[2] + 7;

		type_size = strtoul(p, &q, 10);
		if (q != (p + strlen(p))) {
			fr_strerror_printf("Found trailing garbage '%s' after format specifier", p);
			return -1;
		}
	}

	/*
	 *	Cross check name / number.
	 */
	dict = fr_dict_by_protocol_name(argv[0]);
	if (dict) {
#ifdef __clang_analyzer__
		if (!dict->root) return -1;
#endif

		if (dict->root->attr != value) {
			fr_strerror_printf("Conflicting numbers %u vs %u for PROTOCOL \"%s\"",
					   dict->root->attr, value, dict->root->name);
			return -1;
		}

	} else if ((dict = fr_dict_by_protocol_num(value)) != NULL) {
#ifdef __clang_analyzer__
		if (!dict->root || !dict->root->name || !argv[0]) return -1;
#endif

		if (strcasecmp(dict->root->name, argv[0]) != 0) {
			fr_strerror_printf("Conflicting names current \"%s\" vs new \"%s\" for PROTOCOL %u",
					   dict->root->name, argv[0], dict->root->attr);
			return -1;
		}
	}

	/*
	 *	And check types no matter what.
	 */
	if (dict) {
		if (dict->root->flags.type_size != type_size) {
			fr_strerror_printf("Conflicting flags for PROTOCOL \"%s\" (current %d versus new %d)",
					   dict->root->name, dict->root->flags.type_size, type_size);
			return -1;
		}
		return 0;
	}

	dict = dict_alloc(NULL);

	/*
	 *	Set the root attribute with the protocol name
	 */
	dict_root_set(dict, argv[0], value);

	memcpy(&mutable, &dict->root, sizeof(mutable));
	mutable->flags.type_size = type_size;

	if (dict_protocol_add(dict) < 0) return -1;

	return 0;
}

/*
 *	Process the VENDOR command
 */
static int dict_read_process_vendor(fr_dict_t *dict, char **argv, int argc)
{
	unsigned int			value;
	int				type, length;
	bool				continuation = false;
	fr_dict_vendor_t const		*dv;
	fr_dict_vendor_t		*mutable;

	if ((argc < 2) || (argc > 3)) {
		fr_strerror_printf("Invalid VENDOR syntax");
		return -1;
	}

	/*
	 *	 Validate all entries
	 */
	if (!dict_read_sscanf_i(&value, argv[1])) {
		fr_strerror_printf("Invalid number in VENDOR");
		return -1;
	}

	/*
	 *	Look for a format statement.  Allow it to over-ride the hard-coded formats below.
	 */
	if (argc == 3) {
		if (dict_read_parse_format(argv[2], &value, &type, &length, &continuation) < 0) return -1;

	} else {
		type = length = 1;
	}

	/* Create a new VENDOR entry for the list */
	if (dict_vendor_add(dict, argv[0], value) < 0) return -1;

	dv = fr_dict_vendor_by_num(dict, value);
	if (!dv) {
		fr_strerror_printf("Failed adding format for VENDOR");
		return -1;
	}

	memcpy(&mutable, &dv, sizeof(mutable));

	mutable->type = type;
	mutable->length = length;
	mutable->flags = continuation;

	return 0;
}

/** Empty callback for hash table initialization
 *
 */
static int hash_null_callback(UNUSED void *ctx, UNUSED void *data)
{
	return 0;
}

static int fr_dict_finalise(dict_tokenize_ctx_t *ctx)
{
	/*
	 *	Resolve any VALUE aliases (enums) that were defined
	 *	before the attributes they reference.
	 */
	if (ctx->enum_fixup) {
		fr_dict_attr_t const *da;
		dict_enum_fixup_t *this, *next;

		for (this = ctx->enum_fixup; this != NULL; this = next) {
			fr_value_box_t	value;
			fr_type_t	type;
			int		ret;

			next = this->next;
			da = fr_dict_attr_by_name(ctx->dict, this->attribute);
			if (!da) {
				fr_strerror_printf("No ATTRIBUTE '%s' defined for VALUE '%s' at %s[%d]",
						   this->attribute, this->alias, this->filename, this->line);
				return -1;
			}
			type = da->type;

			if (fr_value_box_from_str(this, &value, &type, NULL,
						  this->value, talloc_array_length(this->value) - 1, '\0', false) < 0) {
				fr_strerror_printf_push("Invalid VALUE for ATTRIBUTE \"%s\" at %s[%d]",
							da->name, this->filename, this->line);
				return -1;
			}

			ret = fr_dict_enum_add_alias(da, this->alias, &value, false, false);
			fr_value_box_clear(&value);

			if (ret < 0) return -1;

			/*
			 *	Just so we don't lose track of things.
			 */
			ctx->enum_fixup = next;
		}
	}

	if (ctx->group_fixup) {
		dict_group_fixup_t *mine, *this, *next;

		mine = ctx->group_fixup;
		ctx->group_fixup = NULL;

		/*
		 *	Loop over references, adding the dictionaries
		 *	and attributes to the da.
		 *
		 *	We avoid refcount loops by using the "autoref"
		 *	table.  If a "group" attribute refers to a
		 *	dictionary which does not exist, we load it,
		 *	increment its reference count, and add it to
		 *	the autoref table.
		 *
		 *	If a group attribute refers to a dictionary
		 *	which does exist, we check that dictionaries
		 *	"autoref" table.  If OUR dictionary is there,
		 *	then we do nothing else.  That dictionary
		 *	points to us via refcounts, so we can safely
		 *	point to it.  The refcounts ensure that we
		 *	won't be free'd before the other one is
		 *	free'd.
		 *
		 *	If our dictionary is NOT in the other
		 *	dictionaries autoref table, then it was loaded
		 *	via some other method.  We increment its
		 *	refcount, and add it to our autoref table.
		 *
		 *	Then when this dictionary is being free'd, we
		 *	also free the dictionaries in our autoref
		 *	table.
		 */
		for (this = mine; this != NULL; this = next) {
			fr_dict_t const *dict;
			fr_dict_attr_t const *da;
			char *p;
			ssize_t slen;

			da = fr_dict_attr_by_name(ctx->dict, this->ref);
			if (da) {
				dict = ctx->dict;
				goto check;
			}

			/*
			 *	The attribute doesn't exist, and the reference
			 *	isn't in a "PROTO.ATTR" format, die.
			 */
			p = strchr(this->ref, '.');

			/*
			 *	Get / skip protocol name.
			 */
			slen = fr_dict_by_protocol_substr(&dict, this->ref, ctx->dict);
			if (slen <= 0) {
				fr_dict_t *other;

				if (p) *p = '\0';

				if (fr_dict_protocol_afrom_file(&other, this->ref, NULL) < 0) {
					return -1;
				}

				if (p) *p = '.';

				/*
				 *	Grab the protocol name again
				 */
				dict = other;
				if (!p) {
					dict = other;
					da = other->root;
					goto check;
				}

				slen = p - this->ref;
			}

			if (slen < 0) {
			invalid_reference:
				fr_strerror_printf("Invalid reference '%s' at %s[%d]",
						 this->ref, this->filename, this->line);
			group_error:
				/*
				 *	Just so we don't lose track of things.
				 */
				// @todo - don't leak group_fixup stuff? things?
				return -1;
			}

			/*
			 *	No known dictionary, so we're asked to just
			 *	use the whole string.  Which we did above.  So
			 *	either it's a bad ref, OR it's a ref to a
			 *	dictionary which doesn't exist.
			 */
			if (slen == 0) goto invalid_reference;

			/*
			 *	Look up the attribute.
			 */
			da = fr_dict_attr_by_name(dict, this->ref + slen + 1);
			if (!da) {
				fr_strerror_printf("No such attribute '%s' in reference at %s[%d]",
						   this->ref + slen + 1, this->filename, this->line);
				goto group_error;
			}

		check:
			if (da->type != FR_TYPE_TLV) {
				fr_strerror_printf("References MUST be to attributes of type 'tlv' at %s[%d]",
					this->filename, this->line);
				goto group_error;
			}

			talloc_free(this->ref);
			this->da->dict = dict;
			this->da->ref = da;

			next = this->next;;
		}
	}

	TALLOC_FREE(ctx->fixup_pool);

	/*
	 *	Walk over all of the hash tables to ensure they're
	 *	initialized.  We do this because the threads may perform
	 *	lookups, and we don't want multi-threaded re-ordering
	 *	of the table entries.  That would be bad.
	 */
	fr_hash_table_walk(ctx->dict->vendors_by_name, hash_null_callback, NULL);
	fr_hash_table_walk(ctx->dict->vendors_by_num, hash_null_callback, NULL);

	fr_hash_table_walk(ctx->dict->values_by_da, hash_null_callback, NULL);
	fr_hash_table_walk(ctx->dict->values_by_alias, hash_null_callback, NULL);

	ctx->value_attr = NULL;
	ctx->relative_attr = NULL;

	return 0;
}

/** Parse a dictionary file
 *
 * @param[in] ctx	Contains the current state of the dictionary parser.
 *			Used to track what PROTOCOL, VENDOR or TLV block
 *			we're in. Block context changes in $INCLUDEs should
 *			not affect the context of the including file.
 * @param[in] dir_name	Directory containing the dictionary we're loading.
 * @param[in] filename	we're parsing.
 * @param[in] src_file	The including file.
 * @param[in] src_line	Line on which the $INCLUDE or $INCLUDE- statement was found.
 * @return
 *	- 0 on success.
 *	- -1 on failure.
 */
static int _dict_from_file(dict_tokenize_ctx_t *ctx,
			   char const *dir_name, char const *filename,
			   char const *src_file, int src_line)
{
	FILE			*fp;
	char 			dir[256], fn[256];
	char			buf[256];
	char			*p;
	int			line = 0;

	struct stat		statbuf;
	char			*argv[MAX_ARGV];
	int			argc;
	fr_dict_attr_t const	*da;

	/*
	 *	Base flags are only set for the current file
	 */
	fr_dict_attr_flags_t	base_flags;

	if (!fr_cond_assert(!ctx->dict->root || ctx->stack[ctx->stack_depth].da)) return -1;

	if ((strlen(dir_name) + 3 + strlen(filename)) > sizeof(dir)) {
		fr_strerror_printf_push("%s: Filename name too long", "Error reading dictionary");
		return -1;
	}

	/*
	 *	If it's an absolute dir, forget the parent dir,
	 *	and remember the new one.
	 *
	 *	If it's a relative dir, tack on the current filename
	 *	to the parent dir.  And use that.
	 */
	if (!FR_DIR_IS_RELATIVE(filename)) {
		strlcpy(dir, filename, sizeof(dir));
		p = strrchr(dir, FR_DIR_SEP);
		if (p) {
			p[1] = '\0';
		} else {
			strlcat(dir, "/", sizeof(dir));
		}

		strlcpy(fn, filename, sizeof(fn));
	} else {
		strlcpy(dir, dir_name, sizeof(dir));
		p = strrchr(dir, FR_DIR_SEP);
		if (p) {
			if (p[1]) strlcat(dir, "/", sizeof(dir));
		} else {
			strlcat(dir, "/", sizeof(dir));
		}
		strlcat(dir, filename, sizeof(dir));
		p = strrchr(dir, FR_DIR_SEP);
		if (p) {
			p[1] = '\0';
		} else {
			strlcat(dir, "/", sizeof(dir));
		}

		p = strrchr(filename, FR_DIR_SEP);
		if (p) {
			snprintf(fn, sizeof(fn), "%s%s", dir, p);
		} else {
			snprintf(fn, sizeof(fn), "%s%s", dir, filename);
		}
	}

	ctx->stack[ctx->stack_depth].filename = fn;

	if ((fp = fopen(fn, "r")) == NULL) {
		if (!src_file) {
			fr_strerror_printf_push("Couldn't open dictionary %s: %s", fr_syserror(errno), fn);
		} else {
			fr_strerror_printf_push("Error reading dictionary: %s[%d]: Couldn't open dictionary '%s': %s",
						src_file, src_line, fn,
						fr_syserror(errno));
		}
		return -2;
	}

	/*
	 *	If fopen works, this works.
	 */
	if (stat(fn, &statbuf) < 0) {
		fclose(fp);
		return -1;
	}

	if (!S_ISREG(statbuf.st_mode)) {
		fclose(fp);
		fr_strerror_printf_push("Dictionary is not a regular file: %s", fn);
		return -1;
	}

	/*
	 *	Globally writable dictionaries means that users can control
	 *	the server configuration with little difficulty.
	 */
#ifdef S_IWOTH
	if ((statbuf.st_mode & S_IWOTH) != 0) {
		fclose(fp);
		fr_strerror_printf_push("Dictionary is globally writable: %s. "
					"Refusing to start due to insecure configuration", fn);
		return -1;
	}
#endif

	/*
	 *	Seed the random pool with data.
	 */
	fr_rand_seed(&statbuf, sizeof(statbuf));

	memset(&base_flags, 0, sizeof(base_flags));

	while (fgets(buf, sizeof(buf), fp) != NULL) {
		ctx->stack[ctx->stack_depth].line = line++;

		switch (buf[0]) {
		case '#':
		case '\0':
		case '\n':
		case '\r':
			continue;
		}

		/*
		 *  Comment characters should NOT be appearing anywhere but
		 *  as start of a comment;
		 */
		p = strchr(buf, '#');
		if (p) *p = '\0';

		argc = fr_dict_str_to_argv(buf, argv, MAX_ARGV);
		if (argc == 0) continue;

		if (argc == 1) {
			fr_strerror_printf("Invalid entry");

		error:
			fr_strerror_printf_push("Error reading %s[%d]", fn, line);
			fclose(fp);
			return -1;
		}

		/*
		 *	Perhaps this is an attribute.
		 */
		if (strcasecmp(argv[0], "ATTRIBUTE") == 0) {
			if (dict_read_process_attribute(ctx,
							argv + 1, argc - 1,
							&base_flags) == -1) goto error;
			continue;
		}

		/*
		 *	Process VALUE lines.
		 */
		if (strcasecmp(argv[0], "VALUE") == 0) {
			if (dict_read_process_value(ctx, argv + 1, argc - 1) == -1) goto error;
			continue;
		}

		/*
		 *	Process FLAGS lines.
		 */
		if (strcasecmp(argv[0], "FLAGS") == 0) {
			if (dict_read_process_flags(ctx->dict, argv + 1, argc - 1, &base_flags) == -1) goto error;
			continue;
		}

		/*
		 *	Perhaps this is a MEMBER of a struct
		 *
		 *	@todo - create child ctx, so that we can have
		 *	nested structs.
		 */
		if (strcasecmp(argv[0], "MEMBER") == 0) {
			if (dict_read_process_member(ctx,
						     argv + 1, argc - 1,
						     &base_flags) == -1) goto error;
			continue;
		}

		/*
		 *	Process STRUCT lines.
		 */
		if (strcasecmp(argv[0], "STRUCT") == 0) {
			if (dict_read_process_struct(ctx, argv + 1, argc - 1) == -1) goto error;
			continue;
		}

		/*
		 *	See if we need to import another dictionary.
		 */
		if (strncasecmp(argv[0], "$INCLUDE", 8) == 0) {
			int rcode;
			int stack_depth = ctx->stack_depth;

			/*
			 *	Allow "$INCLUDE" or "$INCLUDE-", but
			 *	not anything else.
			 */
			if ((argv[0][8] != '\0') && ((argv[0][8] != '-') || (argv[0][9] != '\0'))) goto invalid_keyword;

			/*
			 *	Included files operate on a copy of the context.
			 *
			 *	This copy means that they inherit the
			 *	current context, including parents,
			 *	TLVs, etc.  But if the included file
			 *	leaves a "dangling" TLV or "last
			 *	attribute", then it won't affect the
			 *	parent.
			 */

			rcode = _dict_from_file(ctx, dir, argv[1], fn, line);
			if ((rcode == -2) && (argv[0][8] == '-')) {
				fr_strerror_printf(NULL); /* delete all errors */
				rcode = 0;
			}

			if (rcode < 0) {
				fr_strerror_printf_push("from $INCLUDE at %s[%d]", fn, line);
				fclose(fp);
				return -1;
			}

			if (ctx->stack_depth < stack_depth) {
				fr_strerror_printf_push("unexpected END-??? in $INCLUDE at %s[%d]", fn, line);
				fclose(fp);
				return -1;
			}

			while (ctx->stack_depth > stack_depth) {
				if (ctx->stack[ctx->stack_depth].nest == FR_TYPE_INVALID) {
					ctx->stack_depth--;
					continue;
				}

				fr_strerror_printf_push("BEGIN-??? without END-... in file $INCLUDEd from %s[%d]", fn, line);
				fclose(fp);
				return -1;
			}

			/*
			 *	Reset the filename.
			 */
			ctx->stack[ctx->stack_depth].filename = fn;
			continue;
		} /* $INCLUDE */

		/*
		 *	Reset the previous attribute when we see
		 *	VENDOR or PROTOCOL or BEGIN/END-VENDOR, etc.
		 */
		ctx->value_attr = NULL;
		ctx->relative_attr = NULL;

		/*
		 *	Process VENDOR lines.
		 */
		if (strcasecmp(argv[0], "VENDOR") == 0) {
			if (dict_read_process_vendor(ctx->dict, argv + 1, argc - 1) == -1) goto error;
			continue;
		}

		/*
		 *	Process PROTOCOL line.  Defines a new protocol.
		 */
		if (strcasecmp(argv[0], "PROTOCOL") == 0) {
			if (argc < 2) {
				fr_strerror_printf_push("Invalid PROTOCOL entry");
				goto error;
			}
			if (dict_read_process_protocol(argv + 1, argc - 1) == -1) goto error;
			continue;
		}

		/*
		 *	Switches the current protocol context
		 */
		if (strcasecmp(argv[0], "BEGIN-PROTOCOL") == 0) {
			fr_dict_t *found;

			if (argc != 2) {
				fr_strerror_printf_push("Invalid BEGIN-PROTOCOL entry");
				goto error;
			}

			/*
			 *	If we're not parsing in the context of the internal
			 *	dictionary, then we don't allow BEGIN-PROTOCOL
			 *	statements.
			 */
			if (ctx->dict != fr_dict_internal) {
				fr_strerror_printf_push("Nested BEGIN-PROTOCOL statements are not allowed");
				goto error;
			}

			found = fr_dict_by_protocol_name(argv[1]);
			if (!found) {
				fr_strerror_printf("Unknown protocol '%s'", argv[1]);
				goto error;
			}

			/*
			 *	Add a temporary fixup pool
			 *
			 *	@todo - make a nested ctx?
			 */
			if (!ctx->fixup_pool) ctx->fixup_pool = talloc_pool(NULL, DICT_FIXUP_POOL_SIZE);


			// check if there's a linked library for the
			// protocol.  The values can be unknown (we
			// try to load one), or non-existent, or
			// known.  For the last two, we don't try to
			// load anything.

			ctx->dict = found;

			if (dict_ctx_push(ctx, ctx->dict->root) < 0) goto error;
			ctx->stack[ctx->stack_depth].nest = FR_TYPE_MAX;
			continue;
		}

		/*
		 *	Switches back to the previous protocol context
		 */
		if (strcasecmp(argv[0], "END-PROTOCOL") == 0) {
			fr_dict_t const *found;

			if (argc != 2) {
				fr_strerror_printf("Invalid END-PROTOCOL entry");
				goto error;
			}

			found = fr_dict_by_protocol_name(argv[1]);
			if (!found) {
				fr_strerror_printf("END-PROTOCOL %s does not refer to a valid protocol", argv[1]);
				goto error;
			}

			if (found != ctx->dict) {
				fr_strerror_printf("END-PROTOCOL %s does not match previous BEGIN-PROTOCOL %s",
						   argv[1], found->root->name);
				goto error;
			}

			/*
			 *	Pop the stack until we get to a PROTOCOL nesting.
			 */
			while ((ctx->stack_depth > 0) && (ctx->stack[ctx->stack_depth].nest != FR_TYPE_MAX)) {
				if (ctx->stack[ctx->stack_depth].nest != FR_TYPE_INVALID) {
					fr_strerror_printf_push("END-PROTOCOL %s with mismatched BEGIN-??? %s", argv[1],
						ctx->stack[ctx->stack_depth].da->name);
					goto error;
				}

				ctx->stack_depth--;
			}

			if (ctx->stack_depth == 0) {
				fr_strerror_printf_push("END-PROTOCOL %s with no previous BEGIN-PROTOCOL", argv[1]);
				goto error;
			}

			if (found->root != ctx->stack[ctx->stack_depth].da) {
				fr_strerror_printf_push("END-PROTOCOL %s does not match previous BEGIN-PROTOCOL %s", argv[1],
							ctx->stack[ctx->stack_depth].da->name);
				goto error;
			}

			/*
			 *	Applies fixups to any attributes added
			 *	to the protocol dictionary.  Note that
			 *	the finalise function prints out the
			 *	original filename / line of the
			 *	error. So we don't need to do that
			 *	here.
			 */
			if (fr_dict_finalise(ctx) < 0) {
				fclose(fp);
				return -1;
			}

			ctx->stack_depth--;
			ctx->dict = ctx->stack[ctx->stack_depth].dict;
			continue;
		}

		/*
		 *	Switches TLV parent context
		 */
		if (strcasecmp(argv[0], "BEGIN-TLV") == 0) {
			fr_dict_attr_t const *common;

			if (argc != 2) {
				fr_strerror_printf_push("Invalid BEGIN-TLV entry");
				goto error;
			}

			da = fr_dict_attr_by_name(ctx->dict, argv[1]);
			if (!da) {
				fr_strerror_printf_push("Unknown attribute '%s'", argv[1]);
				goto error;
			}

			if (da->type != FR_TYPE_TLV) {
				fr_strerror_printf_push("Attribute '%s' should be a 'tlv', but is a '%s'",
							argv[1],
							fr_table_str_by_value(fr_value_box_type_table, da->type, "?Unknown?"));
				goto error;
			}

			common = fr_dict_parent_common(ctx->stack[ctx->stack_depth].da, da, true);
			if (!common ||
			    (common->type == FR_TYPE_VSA)) {
				fr_strerror_printf_push("Attribute '%s' should be a child of '%s'",
							argv[1], ctx->stack[ctx->stack_depth].da->name);
				goto error;
			}

			if (dict_ctx_push(ctx, da) < 0) goto error;
			ctx->stack[ctx->stack_depth].nest = FR_TYPE_TLV;
			continue;
		} /* BEGIN-TLV */

		/*
		 *	Switches back to previous TLV parent
		 */
		if (strcasecmp(argv[0], "END-TLV") == 0) {
			if (argc != 2) {
				fr_strerror_printf_push("Invalid END-TLV entry");
				goto error;
			}

			da = fr_dict_attr_by_name(ctx->dict, argv[1]);
			if (!da) {
				fr_strerror_printf_push("Unknown attribute '%s'", argv[1]);
				goto error;
			}

			/*
			 *	Pop the stack until we get to a TLV nesting.
			 */
			while ((ctx->stack_depth > 0) && (ctx->stack[ctx->stack_depth].nest != FR_TYPE_TLV)) {
				if (ctx->stack[ctx->stack_depth].nest != FR_TYPE_INVALID) {
					fr_strerror_printf_push("END-TLV %s with mismatched BEGIN-??? %s", argv[1],
						ctx->stack[ctx->stack_depth].da->name);
					goto error;
				}

				ctx->stack_depth--;
			}

			if (ctx->stack_depth == 0) {
				fr_strerror_printf_push("END-TLV %s with no previous BEGIN-TLV", argv[1]);
				goto error;
			}

			if (da != ctx->stack[ctx->stack_depth].da) {
				fr_strerror_printf_push("END-TLV %s does not match previous BEGIN-TLV %s", argv[1],
							ctx->stack[ctx->stack_depth].da->name);
				goto error;
			}

			ctx->stack_depth--;
			continue;
		} /* END-VENDOR */

		if (strcasecmp(argv[0], "BEGIN-VENDOR") == 0) {
			fr_dict_vendor_t const	*vendor;
			fr_dict_attr_flags_t	flags;

			fr_dict_attr_t const	*vsa_da;
			fr_dict_attr_t const	*vendor_da;
			fr_dict_attr_t		*new;
			fr_dict_attr_t		*mutable;

			if (argc < 2) {
				fr_strerror_printf_push("Invalid BEGIN-VENDOR entry");
				goto error;
			}

			vendor = fr_dict_vendor_by_name(ctx->dict, argv[1]);
			if (!vendor) {
				fr_strerror_printf_push("Unknown vendor '%s'", argv[1]);
				goto error;
			}

			/*
			 *	Check for extended attr VSAs
			 *
			 *	BEGIN-VENDOR foo format=Foo-Encapsulation-Attr
			 */
			if (argc > 2) {
				if (strncmp(argv[2], "format=", 7) != 0) {
					fr_strerror_printf_push("Invalid format %s", argv[2]);
					goto error;
				}

				p = argv[2] + 7;
				da = fr_dict_attr_by_name(ctx->dict, p);
				if (!da) {
					fr_strerror_printf_push("Invalid format for BEGIN-VENDOR: Unknown "
								"attribute '%s'", p);
					goto error;
				}

				if (da->type != FR_TYPE_VSA) {
					fr_strerror_printf_push("Invalid format for BEGIN-VENDOR.  "
								"Attribute '%s' should be 'vsa' but is '%s'", p,
								fr_table_str_by_value(fr_value_box_type_table, da->type, "?Unknown?"));
					goto error;
				}

				if (da->parent->type != FR_TYPE_EXTENDED) {
					fr_strerror_printf_push("Invalid format for BEGIN-VENDOR.  "
								"Attribute '%s' should be parented from an attribute of type 'extended', but is '%s'", p,
								fr_table_str_by_value(fr_value_box_type_table, da->type, "?Unknown?"));
					goto error;
				}

				vsa_da = da;
			} else {
				/*
				 *	Automagically create Attribute 26
				 *
				 *	This should exist, but in case we're starting without
				 *	the RFC dictionaries we need to add it in the case
				 *	it doesn't.
				 */
				vsa_da = fr_dict_attr_child_by_num(ctx->stack[ctx->stack_depth].da, FR_VENDOR_SPECIFIC);
				if (!vsa_da) {
					memset(&flags, 0, sizeof(flags));

					if (fr_dict_attr_add(ctx->dict, ctx->stack[ctx->stack_depth].da, "Vendor-Specific",
							     FR_VENDOR_SPECIFIC, FR_TYPE_VSA, &flags) < 0) {
						fr_strerror_printf_push("Failed adding Vendor-Specific for Vendor %s",
									vendor->name);
						goto error;
					}

					vsa_da = fr_dict_attr_child_by_num(ctx->stack[ctx->stack_depth].da, FR_VENDOR_SPECIFIC);
					if (!vsa_da) {
						fr_strerror_printf_push("Failed finding Vendor-Specific for Vendor %s",
									vendor->name);
						goto error;
					}
				}
			}

			/*
			 *	Create a VENDOR attribute on the fly, either in the context
			 *	of the VSA (26) attribute.
			 */
			vendor_da = fr_dict_attr_child_by_num(vsa_da, vendor->pen);
			if (!vendor_da) {
				memset(&flags, 0, sizeof(flags));

				/*
				 *	Default to 1,1
				 */
				flags.type_size = 1;
				flags.length = 1;

				if ((vsa_da->type == FR_TYPE_VSA) &&
				    (vsa_da->parent->type != FR_TYPE_EXTENDED)) {
					fr_dict_vendor_t const *dv;

					dv = fr_dict_vendor_by_num(ctx->dict, vendor->pen);
					if (dv) {
						flags.type_size = dv->type;
						flags.length = dv->length;

					} else { /* unknown vendor, shouldn't happen */
						flags.type_size = 1;
						flags.length = 1;
					}
				}

				memcpy(&mutable, &vsa_da, sizeof(mutable));
				new = dict_attr_alloc(mutable, ctx->stack[ctx->stack_depth].da, argv[1],
						      vendor->pen, FR_TYPE_VENDOR, &flags);
				if (dict_attr_child_add(mutable, new) < 0) {
					talloc_free(new);
					goto error;
				}

				vendor_da = new;
			}

			if (dict_ctx_push(ctx, vendor_da) < 0) goto error;
			ctx->stack[ctx->stack_depth].nest = FR_TYPE_VENDOR;
			continue;
		} /* BEGIN-VENDOR */

		if (strcasecmp(argv[0], "END-VENDOR") == 0) {
			fr_dict_vendor_t const *vendor;

			if (argc != 2) {
				fr_strerror_printf_push("Invalid END-VENDOR entry");
				goto error;
			}

			vendor = fr_dict_vendor_by_name(ctx->dict, argv[1]);
			if (!vendor) {
				fr_strerror_printf_push("Unknown vendor '%s'", argv[1]);
				goto error;
			}

			/*
			 *	Pop the stack until we get to a VENDOR nesting.
			 */
			while ((ctx->stack_depth > 0) && (ctx->stack[ctx->stack_depth].nest != FR_TYPE_VENDOR)) {
				if (ctx->stack[ctx->stack_depth].nest != FR_TYPE_INVALID) {
					fr_strerror_printf_push("END-VENDOR %s with mismatched BEGIN-??? %s", argv[1],
						ctx->stack[ctx->stack_depth].da->name);
					goto error;
				}

				ctx->stack_depth--;
			}

			if (ctx->stack_depth == 0) {
				fr_strerror_printf_push("END-VENDOR %s with no previous BEGIN-VENDOR", argv[1]);
				goto error;
			}

			if (vendor->pen != ctx->stack[ctx->stack_depth].da->attr) {
				fr_strerror_printf_push("END-VENDOR %s does not match previous BEGIN-VENDOR %s", argv[1],
							ctx->stack[ctx->stack_depth].da->name);
				goto error;
			}

			ctx->stack_depth--;
			continue;
		} /* END-VENDOR */

		/*
		 *	Any other string: We don't recognize it.
		 */
	invalid_keyword:
		fr_strerror_printf_push("Invalid keyword '%s'", argv[0]);
		goto error;
	}

	/*
	 *	Note that we do NOT walk back up the stack to check
	 *	for missing END-FOO to match BEGIN-FOO.  The context
	 *	was copied from the parent, so there are guaranteed to
	 *	be missing things.
	 */

	fclose(fp);
	return 0;
}

static int dict_from_file(fr_dict_t *dict,
			  char const *dir_name, char const *filename,
			  char const *src_file, int src_line)
{
	int rcode;
	dict_tokenize_ctx_t ctx;

	memset(&ctx, 0, sizeof(ctx));
	ctx.dict = dict;
	ctx.stack[0].dict = dict;
	ctx.stack[0].da = dict->root;
	ctx.stack[0].nest = FR_TYPE_MAX;

	rcode = _dict_from_file(&ctx,
				dir_name, filename, src_file, src_line);
	if (rcode < 0) {
		// free up the various fixups
		return rcode;
	}

	/*
	 *	Applies  to any attributes added to the *internal*
	 *	dictionary.
	 *
	 *	Fixups should have been applied already to any protocol
	 *	dictionaries.
	 */
	return fr_dict_finalise(&ctx);
}

/** (Re-)Initialize the special internal dictionary
 *
 * This dictionary has additional programatically generated attributes added to it,
 * and is checked in addition to the protocol specific dictionaries.
 *
 * @note The dictionary pointer returned in out must have its reference counter
 *	 decremented with #fr_dict_free when no longer used.
 *
 * @param[out] out		Where to write pointer to the internal dictionary.
 * @param[in] dict_subdir	name of the internal dictionary dir (may be NULL).
 * @return
 *	- 0 on success.
 *	- -1 on failure.
 */
int fr_dict_internal_afrom_file(fr_dict_t **out, char const *dict_subdir)
{
	fr_dict_t		*dict;
	char			*dict_path = NULL;
	char			*tmp;
	size_t			i;
	fr_dict_attr_flags_t	flags = { .internal = true };
	char			*type_name;

	if (unlikely(!dict_initialised)) {
		fr_strerror_printf("fr_dict_global_init() must be called before loading dictionary files");
		return -1;
	}

	/*
	 *	Increase the reference count of the internal dictionary.
	 */
	if (fr_dict_internal) {
		 talloc_increase_ref_count(fr_dict_internal);
		 *out = fr_dict_internal;
		 return 0;
	}

	memcpy(&tmp, &dict_dir_default, sizeof(tmp));
	dict_path = dict_subdir ? talloc_asprintf(NULL, "%s%c%s", dict_dir_default, FR_DIR_SEP, dict_subdir) : tmp;

	dict = dict_alloc(dict_ctx);
	if (!dict) {
	error:
		if (!fr_dict_internal) talloc_free(dict);
		talloc_free(dict_path);
		return -1;
	}

	/*
	 *	Set the root name of the dictionary
	 */
	dict_root_set(dict, "internal", 0);

	/*
	 *	Add cast attributes.  We do it this way,
	 *	so cast attributes get added automatically for new types.
	 *
	 *	We manually add the attributes to the dictionary, and bypass
	 *	fr_dict_attr_add(), because we know what we're doing, and
	 *	that function does too many checks.
	 */
	for (i = 0; i < fr_value_box_type_table_len; i++) {
		fr_dict_attr_t			*n;
		fr_table_num_ordered_t const	*p = &fr_value_box_type_table[i];

		type_name = talloc_typed_asprintf(NULL, "Tmp-Cast-%s", p->name);

		n = dict_attr_alloc(dict->pool, dict->root, type_name,
				    FR_CAST_BASE + p->value, p->value, &flags);
		if (!n) {
			talloc_free(type_name);
			goto error;
		}

		if (!fr_hash_table_insert(dict->attributes_by_name, n)) {
			fr_strerror_printf("Failed inserting \"%s\" into internal dictionary", type_name);
			talloc_free(type_name);
			goto error;
		}

		talloc_free(type_name);

		/*
		 *	Set up parenting for the attribute.
		 */
		if (dict_attr_child_add(dict->root, n) < 0) goto error;
	}

	if (dict_path && dict_from_file(dict, dict_path, FR_DICTIONARY_FILE, NULL, 0) < 0) goto error;

	talloc_free(dict_path);

	*out = dict;
	if (!fr_dict_internal) fr_dict_internal = dict;

	return 0;
}

/** (Re)-initialize a protocol dictionary
 *
 * Initialize the directory, then fix the attr number of all attributes.
 *
 * @param[out] out		Where to write a pointer to the new dictionary.  Will free existing
 *				dictionary if files have changed and *out is not NULL.
 * @param[in] proto_name	that we're loading the dictionary for.
 * @param[in] proto_dir		Explicitly set where to hunt for the dictionary files.  May be NULL.
 * @return
 *	- 0 on success.
 *	- -1 on failure.
 */
int fr_dict_protocol_afrom_file(fr_dict_t **out, char const *proto_name, char const *proto_dir)
{
	char		*dict_dir = NULL;
	fr_dict_t	*dict;

	if (unlikely(!dict_initialised)) {
		fr_strerror_printf("fr_dict_global_init() must be called before loading dictionary files");
		return -1;
	}

	if (unlikely(!fr_dict_internal)) {
		fr_strerror_printf("Internal dictionary must be initialised before loading protocol dictionaries");
		return -1;
	}

	/*
	 *	Increment the reference count if the dictionary
	 *	has already been loaded and return that.
	 */
	dict = fr_dict_by_protocol_name(proto_name);
	if (dict && dict->autoloaded) {
		talloc_increase_ref_count(dict);
		*out = dict;
		return 0;
	}

	if (!proto_dir) {
		dict_dir = talloc_asprintf(NULL, "%s%c%s", dict_dir_default, FR_DIR_SEP, proto_name);
	} else {
		dict_dir = talloc_asprintf(NULL, "%s%c%s", dict_dir_default, FR_DIR_SEP, proto_dir);
	}

	/*
	 *	Start in the context of the internal dictionary,
	 *	and switch to the context of a protocol dictionary
	 *	when we hit a BEGIN-PROTOCOL line.
	 *
	 *	This allows a single file to provide definitions
	 *	for multiple protocols, which'll probably be useful
	 *	at some point.
	 */
	if (dict_from_file(fr_dict_internal, dict_dir, FR_DICTIONARY_FILE, NULL, 0) < 0) {
	error:
		talloc_free(dict_dir);
		return -1;
	}

	/*
	 *	Check the dictionary actually defined the protocol
	 */
	dict = fr_dict_by_protocol_name(proto_name);
	if (!dict) {
		fr_strerror_printf("Dictionary \"%s\" missing \"BEGIN-PROTOCOL %s\" declaration", dict_dir, proto_name);
		goto error;
	}

	talloc_free(dict_dir);

	/*
	 *	If we're autoloading a previously defined dictionary,
	 *	then mark up the dictionary as now autoloaded.
	 */
	if (!dict->autoloaded) {
//		talloc_increase_ref_count(dict);
		dict->autoloaded = true;
	}

	*out = dict;

	return 0;
}

/** Read supplementary attribute definitions into an existing dictionary
 *
 * @param[in] dict	Existing dictionary.
 * @param[in] dir	dictionary is located in.
 * @param[in] filename	of the dictionary.
 * @return
 *	- 0 on success.
 *      - -1 on failure.
 */
int fr_dict_read(fr_dict_t *dict, char const *dir, char const *filename)
{
	INTERNAL_IF_NULL(dict, -1);

	if (!dict->attributes_by_name) {
		fr_strerror_printf("%s: Must call fr_dict_internal_afrom_file() before fr_dict_read()", __FUNCTION__);
		return -1;
	}

	return dict_from_file(dict, dir, filename, NULL, 0);
}

/*
 *	External API for testing
 */
int fr_dict_parse_str(fr_dict_t *dict, char *buf, fr_dict_attr_t const *parent)
{
	int	argc;
	char	*argv[MAX_ARGV];
	int	ret;
	fr_dict_attr_flags_t base_flags;
	dict_tokenize_ctx_t ctx;

	INTERNAL_IF_NULL(dict, -1);

	argc = fr_dict_str_to_argv(buf, argv, MAX_ARGV);
	if (argc == 0) return 0;


	memset(&ctx, 0, sizeof(ctx));
	ctx.dict = dict;
	ctx.stack[0].dict = dict;
	ctx.stack[0].da = dict->root;
	ctx.stack[0].nest = FR_TYPE_MAX;

	ctx.fixup_pool = talloc_pool(NULL, DICT_FIXUP_POOL_SIZE);
	if (!ctx.fixup_pool) return -1;

	if (strcasecmp(argv[0], "VALUE") == 0) {
		if (argc < 4) {
			fr_strerror_printf("VALUE needs at least 4 arguments, got %i", argc);
		error:
			TALLOC_FREE(ctx.fixup_pool);
			return -1;
		}

		if (!fr_dict_attr_by_name(dict, argv[1])) {
			fr_strerror_printf("Attribute \"%s\" does not exist in dictionary \"%s\"",
					   argv[1], dict->root->name);
			goto error;
		}
		ret = dict_read_process_value(&ctx, argv + 1, argc - 1);
		if (ret < 0) goto error;

	} else if (strcasecmp(argv[0], "ATTRIBUTE") == 0) {
		if (parent && (parent != dict->root)) ctx.stack[++ctx.stack_depth].da = parent;

		memset(&base_flags, 0, sizeof(base_flags));

		ret = dict_read_process_attribute(&ctx,
						  argv + 1, argc - 1, &base_flags);
		if (ret < 0) goto error;
	} else if (strcasecmp(argv[0], "VENDOR") == 0) {
		ret = dict_read_process_vendor(dict, argv + 1, argc - 1);
		if (ret < 0) goto error;
	} else {
		fr_strerror_printf("Invalid input '%s'", argv[0]);
		goto error;
	}

	fr_dict_finalise(&ctx);

	return 0;
}
