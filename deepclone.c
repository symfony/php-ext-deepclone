/*
 * deepclone extension: deep-clones any serializable PHP value while
 * preserving copy-on-write for strings and arrays — resulting in lower
 * memory usage and better performance than unserialize(serialize()).
 *
 * Works by converting the value graph to a pure-array representation (only
 * scalars and nested arrays, no objects) and back. This array form is the
 * wire format used by Symfony's VarExporter\DeepCloner, making the extension
 * a transparent drop-in accelerator.
 *
 *   function deepclone_to_array(mixed $value, ?array $allowed_classes = null): array
 *     Traverses a PHP value graph, extracts object properties, tracks
 *     references, and returns a pure-scalar array equivalent to what
 *     Symfony\Component\VarExporter\DeepCloner::toArray() produces.
 *     Leverages copy-on-write for strings and scalar arrays.
 *
 *   function deepclone_from_array(array $data, ?array $allowed_classes = null): mixed
 *     Reconstructs the value graph from such an array, equivalent to
 *     Symfony\Component\VarExporter\DeepCloner::fromArray($data)->clone().
 *     Throws \ValueError on malformed input.
 *
 *   function deepclone_hydrate(object|string $object_or_class,
 *                              array $vars = [],
 *                              int $flags = 0): object
 *     Instantiates a class (or takes an existing object) and sets its
 *     properties — including private, protected, and readonly — via direct
 *     property-slot writes. Replaces Symfony's Hydrator/Instantiator.
 *
 * `$allowed_classes` follows unserialize()'s semantics: null = allow all,
 * [] = allow none, case-insensitive. Closures require `"Closure"` in the list.
 *
 * Typed exceptions (both extending \InvalidArgumentException):
 *   DeepClone\NotInstantiableException — deepclone_to_array / deepclone_hydrate
 *   DeepClone\ClassNotFoundException   — deepclone_from_array / deepclone_hydrate
 * ValueError is thrown on malformed input or disallowed class names.
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "php.h"
#include "php_deepclone.h"
#include "ext/standard/info.h"
#include "ext/standard/php_var.h"
#include "Zend/zend_smart_str.h"
#include "ext/standard/php_incomplete_class.h"
#include "Zend/zend_closures.h"
#include "Zend/zend_exceptions.h"

/* Stack-limit protection requires PHP 8.4+; no-op on older versions. */
#if PHP_VERSION_ID >= 80400
# include "Zend/zend_call_stack.h"
#endif
#if PHP_VERSION_ID >= 80400
# include "Zend/zend_lazy_objects.h"
#endif
#include "Zend/zend_enum.h"
#include "Zend/zend_interfaces.h"
#include "ext/spl/spl_iterators.h"
#include "ext/spl/spl_exceptions.h"
#include "ext/spl/spl_array.h"
#include "ext/spl/spl_observer.h"

/* ext/reflection's class entries are PHPAPI but Debian's php-dev does not
 * ship ext/reflection/php_reflection.h. Forward-declare what we use; the
 * linker resolves the symbols against the loaded PHP binary at runtime. */
extern PHPAPI zend_class_entry *reflector_ptr;
extern PHPAPI zend_class_entry *reflection_type_ptr;
extern PHPAPI zend_class_entry *reflection_property_ptr;

/* ── Compatibility shims for older PHP versions ────────────── */

/* zend_zval_value_name() landed in PHP 8.3 (returns "true"/"false"/"null"
 * /numeric literals as appropriate). On 8.2 fall back to the older
 * zend_zval_type_name() which returns just the type name ("bool", "int", …).
 * Slightly less informative, same printf format. */
#if PHP_VERSION_ID < 80300
# define zend_zval_value_name(zv) zend_zval_type_name(zv)
#endif

#if PHP_VERSION_ID < 80400
/* rebuild_object_properties_internal() was introduced in 8.4 alongside the
 * zend_std_build_properties() refactor. On 8.2/8.3 the equivalent is the
 * older rebuild_object_properties() (no "_internal" suffix). */
# define rebuild_object_properties_internal(obj) rebuild_object_properties(obj)

/* zend_register_internal_class_with_flags() landed in PHP 8.4. The
 * stub-generated registration code calls it for our DeepClone\* exception
 * classes. On 8.2/8.3 fall back to zend_register_internal_class_ex() and set
 * the flags afterwards. We currently always pass 0 flags, so the assignment
 * is a no-op, but we keep it for future-proofing. */
# define zend_register_internal_class_with_flags(ce, parent, flags) \
    dc_register_internal_class_with_flags((ce), (parent), (flags))
static zend_always_inline zend_class_entry *dc_register_internal_class_with_flags(
    zend_class_entry *class_entry, zend_class_entry *parent_ce, uint32_t flags)
{
    zend_class_entry *registered = zend_register_internal_class_ex(class_entry, parent_ce);
    if (flags) {
        registered->ce_flags |= flags;
    }
    return registered;
}

/* Lazy objects landed in PHP 8.4. Pre-8.4 has no such concept, so the
 * "is this a lazy object?" check is always false and we degrade to the
 * normal walk path. */
# define zend_object_is_lazy(obj) (0)

/* Asymmetric visibility (set-only protected/private) landed in PHP 8.4.
 * On older PHP, readonly is the closest equivalent: a public-read property
 * whose writes are forced into the declaring scope. Aliasing PROTECTED_SET
 * to ZEND_ACC_READONLY makes the existing scope-resolution branch route
 * readonly props through the declaring class on 8.2/8.3 — same outcome as
 * the asymmetric-visibility path on 8.4+. PRIVATE_SET has no pre-8.4
 * equivalent and stays 0. */
# ifndef ZEND_ACC_PROTECTED_SET
#  define ZEND_ACC_PROTECTED_SET ZEND_ACC_READONLY
# endif
# ifndef ZEND_ACC_PRIVATE_SET
#  define ZEND_ACC_PRIVATE_SET (0)
# endif
# ifndef ZEND_ACC_VIRTUAL
#  define ZEND_ACC_VIRTUAL (0)
# endif
# ifndef ZEND_VIRTUAL_PROPERTY_OFFSET
#  define ZEND_VIRTUAL_PROPERTY_OFFSET ((uint32_t)-1)
# endif
# ifndef IS_HOOKED_PROPERTY_OFFSET
#  define IS_HOOKED_PROPERTY_OFFSET(offset) (0)
# endif
/* ZEND_ACC_UNINSTANTIABLE composite landed in PHP 8.4. The bitmask is
 * identical across versions; expand explicitly on 8.2/8.3. */
# ifndef ZEND_ACC_UNINSTANTIABLE
#  define ZEND_ACC_UNINSTANTIABLE (\
	ZEND_ACC_INTERFACE | \
	ZEND_ACC_TRAIT | \
	ZEND_ACC_IMPLICIT_ABSTRACT_CLASS | \
	ZEND_ACC_EXPLICIT_ABSTRACT_CLASS | \
	ZEND_ACC_ENUM \
)
# endif
#endif

#if PHP_VERSION_ID >= 80400
# define DC_PROP_HAS_HOOKS(pi) ((pi)->hooks != NULL)
#else
# define DC_PROP_HAS_HOOKS(pi) (0)
#endif

/* Public flags for deepclone_hydrate()'s $flags parameter. Exported as
 * PHP-level constants via deepclone.stub.php; values must match. */
#define DEEPCLONE_HYDRATE_CALL_HOOKS    (1 << 0)
#define DEEPCLONE_HYDRATE_NO_LAZY_INIT  (1 << 1)
#define DEEPCLONE_HYDRATE_MANGLED_VARS  (1 << 2)
#define DEEPCLONE_HYDRATE_PRESERVE_REFS (1 << 3)
#define DEEPCLONE_HYDRATE_FLAGS_MASK \
	(DEEPCLONE_HYDRATE_CALL_HOOKS | DEEPCLONE_HYDRATE_NO_LAZY_INIT | DEEPCLONE_HYDRATE_MANGLED_VARS | DEEPCLONE_HYDRATE_PRESERVE_REFS)

/* The stub-generated header relies on the compat shims above (specifically
 * zend_register_internal_class_with_flags on PHP < 8.4), so it has to be
 * included after this point. */
#include "deepclone_arginfo.h"

/* Check whether the native call stack is about to overflow, the same way
 * ext/standard/var.c guards php_var_serialize_intern against runaway
 * recursion. Mirrors php_serialize_check_stack_limit(): returns true (and
 * throws \Error via zend_call_stack_size_error) when we're too deep.
 * dc_copy_value (the only recursive walker — dc_copy_array always goes
 * through dc_copy_value) calls this at its entry. No-op on PHP < 8.4
 * (see the header-include block above) or on platforms where
 * ZEND_CHECK_STACK_LIMIT is disabled at configure time. */
static zend_always_inline bool dc_check_stack_limit(void)
{
#if PHP_VERSION_ID >= 80400 && defined(ZEND_CHECK_STACK_LIMIT)
	if (UNEXPECTED(zend_call_stack_overflowed(EG(stack_limit)))) {
		zend_call_stack_size_error();
		return true;
	}
#endif
	return false;
}

/* We key ctx->ref_map on raw zend_reference pointers. Pre-hashing is
 * unsafe because zend_hash uses the stored key as a bucket-chain identity
 * (p->h == h), not just for distribution. The ref_map is sparse enough
 * that aligned-pointer collisions don't cause measurable slowdowns. */

/* ── Permanent interned strings for output keys ───────────── */

static zend_string *dc_key_value;
static zend_string *dc_key_classes;
static zend_string *dc_key_object_meta;
static zend_string *dc_key_prepared;
static zend_string *dc_key_mask;
static zend_string *dc_key_properties;
static zend_string *dc_key_resolve;
static zend_string *dc_key_states;
static zend_string *dc_key_refs;
static zend_string *dc_key_ref_masks;

/* Interned strings for property name / key comparisons */
static zend_string *dc_str_trace;
static zend_string *dc_str_error_trace_mangled;     /* "\0Error\0trace" */
static zend_string *dc_str_exception_trace_mangled; /* "\0Exception\0trace" */
static zend_string *dc_str_file_mangled;            /* "\0*\0file" */
static zend_string *dc_str_line_mangled;            /* "\0*\0line" */

/* Class entry for ReflectionGenerator (resolved at MINIT, since
 * php_reflection.h doesn't export it). */
static zend_class_entry *dc_ce_reflection_generator;

/* Class entries for the exceptions thrown by deepclone_to_array() and
 * deepclone_from_array(). Both extend \InvalidArgumentException; their bare
 * message is the offending class or type name. Registered in MINIT via the
 * stub-generated helpers in deepclone_arginfo.h. */
static zend_class_entry *dc_ce_not_instantiable_exception;
static zend_class_entry *dc_ce_class_not_found_exception;

/* ── Forward declarations ───────────────────────────────────── */

typedef struct _dc_ctx dc_ctx;

static void dc_process_object(dc_ctx *ctx, zval *src, zval *dst, zval *mask_dst);

/* ── Reference pool entry ───────────────────────────────────── */

typedef struct {
	zend_reference *ref;          /* the PHP reference (identity key) */
	uint32_t       id;            /* 1-based ref ID */
	uint32_t       count;         /* re-encounter count */
	zval           orig_type;     /* original value for type detection */
	zval           cur_value;     /* original value for unwrap restoration */
	zval           cur_mask;      /* original mask for unwrap restoration */
	zval          *tree_pos;      /* pointer to the dst slot in the prepared tree */
	zval          *mask_slot;     /* pointer to the mask zval for this ref (in parent array) */
} dc_ref_entry;

/* ── Object pool entry ──────────────────────────────────────── */

typedef struct {
	uint32_t       id;
	uint32_t       cidx;          /* class index in the deduped classes[] array */
	zend_string   *class_name;
	bool           class_name_owned; /* true if class_name was allocated (Serializable) */
	int            wakeup;        /* >0 = __wakeup order, <0 = __unserialize order, 0 = none */
	HashTable     *props;         /* [scope][name] => value (already prepared) */
	HashTable     *prop_mask;     /* [scope][name] => mask marker (or NULL) */
} dc_pool_entry;

/* ── Traversal context ──────────────────────────────────────── */

struct _dc_ctx {
	HashTable      object_pool;    /* obj_handle => dc_pool_entry */
	dc_pool_entry **entries;       /* indexed by entry->id (id-ordered iteration) */
	uint32_t       entries_cap;
	dc_ref_entry  *refs;           /* dynamic array */
	uint32_t       refs_count;
	uint32_t       refs_cap;
	HashTable      ref_map;        /* zend_reference* => index in refs[] */
	uint32_t       next_obj_id;
	uint32_t       objects_count;
	bool           is_static;
	HashTable     *allowed_ht;     /* allowed class names set (or NULL = all) */

	/* Output structures built incrementally during traversal */
	zval           classes;        /* deduped class names */
	zval           properties;     /* [scope][name][id] => value */
	zval           resolve;        /* [scope][name][id] => marker */
	HashTable      class_map;      /* class_name => cidx (uint32_t in zval long) */

	/* Scope map cache: class_name => HashTable(prop_name => scope_class_name) */
	HashTable      scope_cache;

	/* Class info cache: class_name => [has_unserialize, has_wakeup, serialize_method, has_sleep] */
	HashTable      class_info;

	/* Proto cache: class_name => (array) prototype */
	HashTable      proto_cache;
};

/* ── Class info cache entry ─────────────────────────────────── */

#define DC_CI_HAS_UNSERIALIZE  (1 << 0)
#define DC_CI_HAS_WAKEUP       (1 << 1)
#define DC_CI_HAS_SERIALIZE    (1 << 2)
#define DC_CI_SERIALIZE_PUBLIC (1 << 3)
#define DC_CI_HAS_SLEEP        (1 << 4)
#define DC_CI_NOT_INSTANTIABLE (1 << 5)
#define DC_CI_COMPUTED         (1 << 7)


/* ── Helpers ────────────────────────────────────────────────── */

static void dc_ctx_init(dc_ctx *ctx) {
	zend_hash_init(&ctx->object_pool, 8, NULL, NULL, 0);
	ctx->entries = NULL;
	ctx->entries_cap = 0;
	ctx->refs = NULL;
	ctx->refs_count = 0;
	ctx->refs_cap = 0;
	ZVAL_UNDEF(&ctx->classes);
	ZVAL_UNDEF(&ctx->properties);
	ZVAL_UNDEF(&ctx->resolve);
	zend_hash_init(&ctx->class_map, 4, NULL, NULL, 0);
	zend_hash_init(&ctx->ref_map, 8, NULL, NULL, 0);
	ctx->next_obj_id = 0;
	ctx->objects_count = 0;
	ctx->is_static = 1;
	ctx->allowed_ht = NULL;
	zend_hash_init(&ctx->scope_cache, 4, NULL, ZVAL_PTR_DTOR, 0);
	zend_hash_init(&ctx->class_info, 4, NULL, NULL, 0);
	zend_hash_init(&ctx->proto_cache, 4, NULL, ZVAL_PTR_DTOR, 0);
}

static void dc_ctx_destroy(dc_ctx *ctx) {
	zend_hash_destroy(&ctx->object_pool);
	zval_ptr_dtor(&ctx->classes);
	zval_ptr_dtor(&ctx->properties);
	zval_ptr_dtor(&ctx->resolve);
	zend_hash_destroy(&ctx->class_map);
	if (ctx->entries) {
		/* Free any remaining pool entries (entries whose props/prop_mask were
		 * transferred into the output have those fields nulled out by
		 * dc_build_output before reaching here). */
		for (uint32_t id = 0; id < ctx->next_obj_id; id++) {
			dc_pool_entry *e = ctx->entries[id];
			if (!e) continue;
			if (e->class_name_owned) {
				zend_string_release(e->class_name);
			}
			if (e->props) {
				zend_array_destroy(e->props);
			}
			if (e->prop_mask) {
				zend_array_destroy(e->prop_mask);
			}
			efree(e);
		}
		efree(ctx->entries);
	}
	if (ctx->refs) {
		for (uint32_t i = 0; i < ctx->refs_count; i++) {
			zval_ptr_dtor(&ctx->refs[i].orig_type);
			zval_ptr_dtor(&ctx->refs[i].cur_value);
			zval_ptr_dtor(&ctx->refs[i].cur_mask);
		}
		efree(ctx->refs);
	}
	zend_hash_destroy(&ctx->ref_map);
	zend_hash_destroy(&ctx->scope_cache);
	zend_hash_destroy(&ctx->class_info);
	zend_hash_destroy(&ctx->proto_cache);
	if (ctx->allowed_ht) {
		zend_hash_destroy(ctx->allowed_ht);
		efree(ctx->allowed_ht);
	}
}

/* Assign or fetch the deduplicated class index for a class name */
static uint32_t dc_class_index(dc_ctx *ctx, zend_string *class_name)
{
	zval *cached = zend_hash_find(&ctx->class_map, class_name);
	if (EXPECTED(cached)) {
		return (uint32_t) Z_LVAL_P(cached);
	}
	if (Z_TYPE(ctx->classes) == IS_UNDEF) {
		array_init_size(&ctx->classes, 1);
	}
	uint32_t cidx = zend_hash_num_elements(Z_ARRVAL(ctx->classes));
	zval zidx;
	ZVAL_LONG(&zidx, cidx);
	zend_hash_add_new(&ctx->class_map, class_name, &zidx);
	zval zclass;
	ZVAL_STR_COPY(&zclass, class_name);
	zend_hash_next_index_insert_new(Z_ARRVAL(ctx->classes), &zclass);
	return cidx;
}

static uint32_t dc_ref_add(dc_ctx *ctx, zend_reference *ref, zval *orig, zval *current) {
	if (ctx->refs_count >= ctx->refs_cap) {
		/* Grow by 1.5× instead of 2× — slightly less memory at high counts
		 * while still amortised O(1). See folly's FBVector rationale. The
		 * +1 covers the cap=1 edge case where (1 * 3) >> 1 == 1 (no growth),
		 * and the `<8` floor keeps the very first allocation at a useful
		 * size without going through two or three grow steps. */
		ctx->refs_cap = ctx->refs_cap < 8 ? 8 : ((ctx->refs_cap * 3) >> 1) + 1;
		ctx->refs = safe_erealloc(ctx->refs, ctx->refs_cap, sizeof(dc_ref_entry), 0);
	}
	uint32_t idx = ctx->refs_count++;
	ctx->refs[idx].ref = ref;
	ctx->refs[idx].id = idx + 1;  /* 1-based */
	ctx->refs[idx].count = 0;
	ZVAL_COPY(&ctx->refs[idx].orig_type, orig);
	ZVAL_COPY(&ctx->refs[idx].cur_value, current);
	ZVAL_UNDEF(&ctx->refs[idx].cur_mask);
	ctx->refs[idx].tree_pos = NULL;
	ctx->refs[idx].mask_slot = NULL;
	/* Map ref pointer → index. See the comment at the top of the file
	 * about not pre-hashing keys handed to zend_hash_index_*. */
	zval zidx;
	ZVAL_LONG(&zidx, idx);
	zend_hash_index_add_new(&ctx->ref_map, (zend_ulong)(uintptr_t)ref, &zidx);
	return idx;
}

static uint8_t dc_get_class_info(dc_ctx *ctx, zend_class_entry *ce)
{
	zval *cached = zend_hash_find(&ctx->class_info, ce->name);

	if (EXPECTED(cached)) {
		return (uint8_t) Z_LVAL_P(cached);
	}

	uint8_t flags = DC_CI_COMPUTED;

	/* Use direct ce fields like native serialize does (O(1) vs hash lookup) */
	if (ce->__unserialize) {
		flags |= DC_CI_HAS_UNSERIALIZE;
	}
	if (ce->__serialize) {
		flags |= DC_CI_HAS_SERIALIZE;
		if (ce->__serialize->common.fn_flags & ZEND_ACC_PUBLIC) {
			flags |= DC_CI_SERIALIZE_PUBLIC;
		}
	}
	/* __sleep and __wakeup have no direct ce field — use known-string lookup */
	if (zend_hash_find_known_hash(&ce->function_table, ZSTR_KNOWN(ZEND_STR_SLEEP))) {
		flags |= DC_CI_HAS_SLEEP;
	}
	if (zend_hash_find_known_hash(&ce->function_table, ZSTR_KNOWN(ZEND_STR_WAKEUP))) {
		flags |= DC_CI_HAS_WAKEUP;
	}

	/* Mark anonymous classes (names tied to file/line, can't round-trip) and
	 * Reflection / IteratorIterator / RecursiveIteratorIterator subclasses as
	 * non-instantiable. Escape hatches: Serializable, __wakeup, __unserialize. */
	if (!(flags & (DC_CI_HAS_UNSERIALIZE | DC_CI_HAS_WAKEUP)) && ce->serialize == NULL
	 && ((ce->ce_flags & ZEND_ACC_ANON_CLASS)
	  || instanceof_function(ce, reflector_ptr)
	  || instanceof_function(ce, reflection_type_ptr)
	  || instanceof_function(ce, spl_ce_IteratorIterator)
	  || instanceof_function(ce, spl_ce_RecursiveIteratorIterator)
	  || (dc_ce_reflection_generator && instanceof_function(ce, dc_ce_reflection_generator)))) {
		flags |= DC_CI_NOT_INSTANTIABLE;
	}

	/* Honour ZEND_ACC_NOT_SERIALIZABLE — classes that explicitly refuse
	 * serialization. Escape hatch: if the class declares its own
	 * (un)serialization API, trust the declaration. */
	if ((ce->ce_flags & ZEND_ACC_NOT_SERIALIZABLE)
	 && !(flags & (DC_CI_HAS_UNSERIALIZE | DC_CI_HAS_WAKEUP))
	 && ce->serialize == NULL) {
		flags |= DC_CI_NOT_INSTANTIABLE;
	}

	/* Internal classes with create_object and no serialization API:
	 * final → probe instantiation (stateless classes like BSON\MinKey pass);
	 * non-final → reject. Classes with __serialize/__unserialize are trusted. */
	if (ce->type == ZEND_INTERNAL_CLASS
	 && ce->create_object != NULL
	 && (ce->ce_flags & ZEND_ACC_FINAL)
	 && !(flags & (DC_CI_HAS_SERIALIZE | DC_CI_HAS_UNSERIALIZE | DC_CI_HAS_SLEEP | DC_CI_HAS_WAKEUP))
	 && ce != php_ce_incomplete_class) {
		zval probe;
		if (object_init_ex(&probe, ce) != SUCCESS || EG(exception)) {
			zend_clear_exception();
			flags |= DC_CI_NOT_INSTANTIABLE;
		} else {
			zval_ptr_dtor(&probe);
		}
	} else if (ce->type == ZEND_INTERNAL_CLASS
	 && ce->create_object != NULL
	 && ce->serialize == NULL
	 && !(flags & (DC_CI_HAS_SERIALIZE | DC_CI_HAS_UNSERIALIZE | DC_CI_HAS_SLEEP | DC_CI_HAS_WAKEUP))
	 && ce != php_ce_incomplete_class) {
		flags |= DC_CI_NOT_INSTANTIABLE;
	}

	zval zflags;
	ZVAL_LONG(&zflags, flags);
	zend_hash_add_new(&ctx->class_info, ce->name, &zflags);
	return flags;
}

/* Get or build the scope map for a class: property_name => declaring_class_name */
static HashTable *dc_get_scope_map(dc_ctx *ctx, zend_class_entry *ce) {
	zval *cached = zend_hash_find(&ctx->scope_cache, ce->name);
	if (EXPECTED(cached)) {
		return Z_ARRVAL_P(cached);
	}
	zval zmap;
	array_init(&zmap);
	HashTable *map = Z_ARRVAL(zmap);

	zend_class_entry *parent = ce;
	while (parent) {
		for (uint32_t i = 0; i < parent->default_properties_count; i++) {
			zend_property_info *pi = parent->properties_info_table[i];
			if (!pi || (pi->flags & ZEND_ACC_STATIC)) continue;

			/* Use unmangled name as key (pi->name is mangled for non-public) */
			zend_string *key;
			if (pi->flags & ZEND_ACC_PUBLIC) {
				key = zend_string_copy(pi->name);
			} else {
				const char *class_name_unused, *uname;
				size_t uname_len;
				zend_unmangle_property_name_ex(pi->name, &class_name_unused, &uname, &uname_len);
				key = zend_string_init_existing_interned(uname, uname_len, 0);
			}

			if (zend_hash_exists(map, key)) {
				zend_string_release(key);
				continue;
			}

			zval zscope;
			if ((pi->flags & ZEND_ACC_PUBLIC) && !(pi->flags & ZEND_ACC_PROTECTED_SET) && !(pi->flags & ZEND_ACC_PRIVATE_SET)) {
				ZVAL_STR_COPY(&zscope, ZEND_STANDARD_CLASS_DEF_PTR->name);
			} else {
				ZVAL_STR_COPY(&zscope, pi->ce->name);
			}
			zend_hash_add_new(map, key, &zscope);
			zend_string_release(key);
		}
		parent = parent->parent;
	}
	zend_hash_add_new(&ctx->scope_cache, ce->name, &zmap);
	return map;
}

/* Get (array) prototype for a class (cached) */
static HashTable *dc_get_proto(dc_ctx *ctx, zend_class_entry *ce) {
	zval *cached = zend_hash_find(&ctx->proto_cache, ce->name);
	if (EXPECTED(cached)) {
		return Z_ARRVAL_P(cached);
	}
	/* Create a prototype instance */
	zval proto_zval;
	if (ce->create_object) {
		zend_object *proto_obj = ce->create_object(ce);
		ZVAL_OBJ(&proto_zval, proto_obj);
	} else {
		object_init_ex(&proto_zval, ce);
	}
	/* Cast to array */
	zval proto_arr;
	HashTable *ht = zend_get_properties_for(&proto_zval, ZEND_PROP_PURPOSE_ARRAY_CAST);
	if (ht) {
		ZVAL_ARR(&proto_arr, zend_array_dup(ht));
		zend_release_properties(ht);
	} else {
		array_init(&proto_arr);
	}
	zval_ptr_dtor(&proto_zval);
	zend_hash_add_new(&ctx->proto_cache, ce->name, &proto_arr);
	return Z_ARRVAL(proto_arr);
}

/* Check if an array contains only scalars/enums (no objects, no refs, no
 * resources). If so, the walker can COW-share it without recursing. */
static bool dc_array_is_static(HashTable *ht)
{
	if (UNEXPECTED(GC_FLAGS(ht) & GC_IMMUTABLE)) {
		return true;
	}
	if (UNEXPECTED(dc_check_stack_limit())) {
		return false;
	}
	zval *val;
	ZEND_HASH_FOREACH_VAL(ht, val) {
		if (UNEXPECTED(Z_ISREF_P(val))) {
			return false;
		}
		if (EXPECTED(Z_TYPE_P(val) <= IS_STRING)) {
			continue;
		}
		if (Z_TYPE_P(val) == IS_ARRAY) {
			if (zend_hash_num_elements(Z_ARRVAL_P(val)) > 0
			 && !dc_array_is_static(Z_ARRVAL_P(val))) {
				return false;
			}
		} else if (Z_TYPE_P(val) == IS_OBJECT) {
			if (!(Z_OBJCE_P(val)->ce_flags & ZEND_ACC_ENUM)) {
				return false;
			}
		} else {
			/* IS_RESOURCE or anything else: force the walker to handle it,
			 * which will reject resources via dc_copy_value. */
			return false;
		}
	} ZEND_HASH_FOREACH_END();
	return true;
}

/* ── Allowed-class validation helpers ──────────────────────── */

/* Build a lowercased name-keyed HashTable from a user-provided list of
 * class names, validating each entry like unserialize() does. Returns
 * the set on success (caller must zend_hash_destroy + efree) or NULL on
 * validation failure (exception already thrown). */
static HashTable *dc_build_allowed_set(HashTable *list, const char *func_name)
{
	HashTable *set = emalloc(sizeof(HashTable));
	zend_hash_init(set, zend_hash_num_elements(list), NULL, NULL, 0);
	zval *entry;
	ZEND_HASH_FOREACH_VAL(list, entry) {
		if (Z_TYPE_P(entry) != IS_STRING) {
			zend_hash_destroy(set);
			efree(set);
			zend_value_error("%s(): Argument $allowedClasses must be an array of class names, %s given",
				func_name, zend_zval_value_name(entry));
			return NULL;
		}
		if (!zend_is_valid_class_name(Z_STR_P(entry))) {
			zend_hash_destroy(set);
			efree(set);
			zend_value_error("%s(): Argument $allowedClasses must be an array of class names, \"%s\" given",
				func_name, ZSTR_VAL(Z_STR_P(entry)));
			return NULL;
		}
		zend_string *lcname = zend_string_tolower(Z_STR_P(entry));
		zend_hash_add_empty_element(set, lcname);
		zend_string_release(lcname);
	} ZEND_HASH_FOREACH_END();
	return set;
}

static zend_always_inline bool dc_class_allowed(HashTable *set, zend_string *name)
{
	if (!set) return true;
	zend_string *lcname = zend_string_tolower(name);
	bool found = zend_hash_exists(set, lcname);
	zend_string_release(lcname);
	return found;
}

static zend_always_inline bool dc_is_backed_declared_property(zend_property_info *pi)
{
	/* A "backed declared property" is a non-static, non-virtual property with
	 * a real backing slot (offset >= ZEND_FIRST_PROPERTY_OFFSET). Hooked
	 * non-virtual properties qualify (they have a real offset); virtual
	 * properties and hook-metadata offsets do not. */
	return pi
		&& !(pi->flags & ZEND_ACC_STATIC)
		&& !(pi->flags & ZEND_ACC_VIRTUAL)
		&& pi->offset != ZEND_VIRTUAL_PROPERTY_OFFSET
		&& !IS_HOOKED_PROPERTY_OFFSET(pi->offset);
}

static zend_always_inline bool dc_is_std_scope_property(zend_property_info *pi)
{
	return pi
		&& !(pi->flags & ZEND_ACC_STATIC)
		&& (pi->flags & ZEND_ACC_PUBLIC)
		&& !(pi->flags & (ZEND_ACC_PROTECTED_SET | ZEND_ACC_PRIVATE_SET));
}

#if PHP_VERSION_ID >= 80400
/* fn_proxy slot cached across calls — first invocation fills it via method
 * lookup; subsequent invocations reuse the resolved zend_function*. */
static zend_function *dc_set_raw_no_lazy_fn = NULL;

/* HashTable destructor for lazy_init_refl_cache (releases cached
 * ReflectionProperty instances). Defined later; forward-declared here. */
static void dc_lazy_refl_cache_dtor(zval *zv);

/* Delegates to ReflectionProperty::setRawValueWithoutLazyInitialization because the
 * required engine helpers (zend_lazy_object_decr_lazy_props, _realize) aren't ZEND_API.
 * Per-request cache keyed on pi — the ReflectionProperty is per-class, not per-instance. */
static bool dc_set_raw_value_without_lazy_init(zend_object *obj,
	zend_property_info *pi, zend_string *name, zval *value)
{
	HashTable *cache = &DC_G(lazy_init_refl_cache);
	zend_object *refl_obj = NULL;

	if (cache->nTableSize) {
		refl_obj = zend_hash_index_find_ptr(cache, (zend_ulong) (uintptr_t) pi);
	}

	if (!refl_obj) {
		zval refl_zv;
		if (UNEXPECTED(object_init_ex(&refl_zv, reflection_property_ptr) != SUCCESS)) {
			return false;
		}

		zval ctor_args[2];
		ZVAL_STR_COPY(&ctor_args[0], pi->ce->name);
		ZVAL_STR_COPY(&ctor_args[1], name);

		zend_call_method_with_2_params(Z_OBJ(refl_zv), reflection_property_ptr,
			&reflection_property_ptr->constructor, "__construct", NULL,
			&ctor_args[0], &ctor_args[1]);

		zval_ptr_dtor(&ctor_args[0]);
		zval_ptr_dtor(&ctor_args[1]);

		if (UNEXPECTED(EG(exception))) {
			zval_ptr_dtor(&refl_zv);
			return false;
		}

		if (!cache->nTableSize) {
			zend_hash_init(cache, 8, NULL, dc_lazy_refl_cache_dtor, 0);
		}
		refl_obj = Z_OBJ(refl_zv);
		zend_hash_index_add_ptr(cache, (zend_ulong) (uintptr_t) pi, refl_obj);
		/* Cache holds the only reference; refcount stays at 1. */
	}

	zval method_args[2];
	ZVAL_OBJ_COPY(&method_args[0], obj);
	ZVAL_COPY(&method_args[1], value);

	zend_call_method_with_2_params(refl_obj, reflection_property_ptr,
		&dc_set_raw_no_lazy_fn,
		"setRawValueWithoutLazyInitialization", NULL,
		&method_args[0], &method_args[1]);

	zval_ptr_dtor(&method_args[0]);
	zval_ptr_dtor(&method_args[1]);

	return !EG(exception);
}
#endif

/* Default dispatch matches ReflectionProperty::setRawValue. The non-obvious branch
 * is the hook trampoline path: zend_std_write_property's recursion check sees
 * prop_info on execute_data->func and short-circuits to the backing write,
 * bypassing the user set hook while keeping the type-check.
 * Caller must have verified dc_is_backed_declared_property(pi). */
static bool dc_write_backed_property(zend_object *obj, zend_property_info *pi,
	zend_string *name, zval *value, zend_long flags)
{
#if PHP_VERSION_ID >= 80400
	bool call_hooks   = (flags & DEEPCLONE_HYDRATE_CALL_HOOKS) != 0;
	bool no_lazy_init = (flags & DEEPCLONE_HYDRATE_NO_LAZY_INIT) != 0;
#endif
	zval *slot = OBJ_PROP(obj, pi->offset);

	/* Idempotent readonly write — readonly and hooks are XOR, so no trampoline concern. */
	if ((pi->flags & ZEND_ACC_READONLY)
		&& Z_TYPE_P(slot) != IS_UNDEF
		&& !(Z_PROP_FLAG_P(slot) & IS_PROP_UNINIT)
		&& zend_is_identical(slot, value))
	{
		return true;
	}

	/* null → uninitialized for non-nullable typed slots; hooked props excluded
	 * (no backing slot to "unset", and the set hook may handle null itself). */
	if (Z_TYPE_P(value) == IS_NULL
		&& ZEND_TYPE_IS_SET(pi->type)
		&& !ZEND_TYPE_ALLOW_NULL(pi->type)
		&& !DC_PROP_HAS_HOOKS(pi))
	{
		if (Z_TYPE_P(slot) != IS_UNDEF) {
			zval old;
			ZVAL_COPY_VALUE(&old, slot);
			ZVAL_UNDEF(slot);
			Z_PROP_FLAG_P(slot) |= IS_PROP_UNINIT;
			zval_ptr_dtor(&old);
		} else {
			Z_PROP_FLAG_P(slot) |= IS_PROP_UNINIT;
		}
		return true;
	}

#if PHP_VERSION_ID >= 80100
	/* Property-type-only decision: hook presence and CALL_HOOKS don't influence it. */
	zval enum_holder;
	bool enum_holder_used = false;
	if ((Z_TYPE_P(value) == IS_LONG || Z_TYPE_P(value) == IS_STRING)
		&& ZEND_TYPE_HAS_NAME(pi->type)
		&& !ZEND_TYPE_HAS_LIST(pi->type))
	{
		zend_class_entry *type_ce = zend_lookup_class_ex(
			ZEND_TYPE_NAME(pi->type), NULL, ZEND_FETCH_CLASS_NO_AUTOLOAD);
		if (type_ce && (type_ce->ce_flags & ZEND_ACC_ENUM)
			&& type_ce->enum_backing_type != IS_UNDEF)
		{
			/* Enum::from() for parity with polyfill: standard TypeError/ValueError, scalar coercion. */
			ZVAL_UNDEF(&enum_holder);
			zend_call_method_with_1_params(NULL, type_ce, NULL, "from",
				&enum_holder, value);
			if (UNEXPECTED(EG(exception))) {
				return false;
			}
			value = &enum_holder;
			enum_holder_used = true;
		}
	}
#endif

#if PHP_VERSION_ID >= 80400
	/* Skip the Reflection round-trip when there's no lazy-init to skip. */
	if (no_lazy_init && !zend_lazy_object_initialized(obj)) {
		bool ok = dc_set_raw_value_without_lazy_init(obj, pi, name, value);
#if PHP_VERSION_ID >= 80100
		if (enum_holder_used) {
			zval_ptr_dtor(&enum_holder);
		}
#endif
		return ok;
	}
#endif

	if (!ZEND_TYPE_IS_SET(pi->type) && !DC_PROP_HAS_HOOKS(pi)) {
		/* Move the old value out before running its destructor: a __destruct
		 * on the old value can legitimately read (or reassign) this same slot.
		 * Install the new value first so reentrant reads see a valid slot. */
		zval old;
		ZVAL_COPY_VALUE(&old, slot);
		ZVAL_COPY(slot, value);
		zval_ptr_dtor(&old);
	}
#if PHP_VERSION_ID >= 80400
	else if (!call_hooks && DC_PROP_HAS_HOOKS(pi) && pi->hooks[ZEND_PROPERTY_HOOK_SET]) {
		zend_function *trampoline = zend_get_property_hook_trampoline(
			pi, ZEND_PROPERTY_HOOK_SET, name);
		zend_call_known_instance_method_with_1_params(trampoline, obj, NULL, value);
	}
#endif
	else {
		zend_update_property_ex(pi->ce, obj, name, value);
	}

#if PHP_VERSION_ID >= 80100
	if (enum_holder_used) {
		zval_ptr_dtor(&enum_holder);
	}
#endif
	return !EG(exception);
}

/* ── Core traversal ─────────────────────────────────────────── */

/* Mask markers: TRUE=obj_ref, FALSE=hard_ref, LONG(0)=named_closure,
 * STRING("e")=enum, ARRAY=nested sub-mask. */
#define DC_MASK_OBJ_REF(m)        ZVAL_TRUE(m)
#define DC_MASK_HARD_REF(m)       ZVAL_FALSE(m)
#define DC_MASK_NAMED_CLOSURE(m)  ZVAL_LONG((m), 0)

/* Test whether a mask slot carries a particular marker. */
#define DC_MASK_IS_OBJ_REF(m)       (Z_TYPE_P(m) == IS_TRUE)
#define DC_MASK_IS_HARD_REF(m)      (Z_TYPE_P(m) == IS_FALSE)
#define DC_MASK_IS_NAMED_CLOSURE(m) (Z_TYPE_P(m) == IS_LONG && Z_LVAL_P(m) == 0)

static void dc_copy_value(dc_ctx *ctx, zval *src, zval *dst, zval *mask_dst);
static void dc_copy_array(dc_ctx *ctx, HashTable *src_ht, zval *dst, zval *mask_dst);

static void dc_copy_array(dc_ctx *ctx, HashTable *src_ht, zval *dst, zval *mask_dst)
{
	zend_string *key;
	zend_ulong idx;
	zval *src_val;
	uint32_t n = zend_hash_num_elements(src_ht);

	array_init_size(dst, n);
	/* Seed mask slots with IS_NULL (not IS_UNDEF — HT iteration skips those). */
	array_init_size(mask_dst, n);

	/* Fast path: packed source without holes → lockstep arPacked walk. */
	if (EXPECTED(HT_IS_PACKED(src_ht) && HT_IS_WITHOUT_HOLES(src_ht))) {
		HashTable *dst_ht = Z_ARRVAL_P(dst);
		HashTable *mask_ht = Z_ARRVAL_P(mask_dst);

		zend_hash_real_init_packed(dst_ht);
		zend_hash_real_init_packed(mask_ht);

		ZEND_HASH_FILL_PACKED(dst_ht) {
			for (uint32_t i = 0; i < n; i++) {
				ZVAL_UNDEF(__fill_val);
				ZEND_HASH_FILL_NEXT();
			}
		} ZEND_HASH_FILL_END();

		ZEND_HASH_FILL_PACKED(mask_ht) {
			for (uint32_t i = 0; i < n; i++) {
				ZEND_HASH_FILL_SET_NULL();
				ZEND_HASH_FILL_NEXT();
			}
		} ZEND_HASH_FILL_END();

		zval *dst_slot = dst_ht->arPacked;
		zval *mask_slot = mask_ht->arPacked;
		ZEND_HASH_PACKED_FOREACH_VAL(src_ht, src_val) {
			dc_copy_value(ctx, src_val, dst_slot, mask_slot);
			if (UNEXPECTED(EG(exception))) return;
			dst_slot++;
			mask_slot++;
		} ZEND_HASH_FOREACH_END();

		return;
	}

	ZEND_HASH_FOREACH_KEY_VAL(src_ht, idx, key, src_val) {
		zval undef, null_marker;
		ZVAL_UNDEF(&undef);
		ZVAL_NULL(&null_marker);
		zval *new_dst_slot, *new_mask_slot;
		if (key) {
			new_dst_slot  = zend_hash_add_new(Z_ARRVAL_P(dst), key, &undef);
			new_mask_slot = zend_hash_add_new(Z_ARRVAL_P(mask_dst), key, &null_marker);
		} else {
			new_dst_slot  = zend_hash_index_add_new(Z_ARRVAL_P(dst), idx, &undef);
			new_mask_slot = zend_hash_index_add_new(Z_ARRVAL_P(mask_dst), idx, &null_marker);
		}
		dc_copy_value(ctx, src_val, new_dst_slot, new_mask_slot);
		if (UNEXPECTED(EG(exception))) return;
	} ZEND_HASH_FOREACH_END();
}

static void dc_mask_cleanup(zval *mask);

/* zend_hash_apply callback: drop IS_NULL placeholders (the seeds dropped by
 * dc_copy_array() that were never overwritten by a real marker, and the slots
 * cleared by the unshared-ref unwrap pass). */
static int dc_mask_cleanup_apply(zval *v)
{
	if (Z_TYPE_P(v) == IS_ARRAY) {
		dc_mask_cleanup(v);
		if (Z_TYPE_P(v) == IS_NULL) {
			return ZEND_HASH_APPLY_REMOVE;
		}
		return ZEND_HASH_APPLY_KEEP;
	}
	return Z_TYPE_P(v) == IS_NULL ? ZEND_HASH_APPLY_REMOVE : ZEND_HASH_APPLY_KEEP;
}

/* Recursively strip the IS_NULL placeholders that dc_copy_array() seeded into
 * the mask buckets. After all real markers have been written, anything still
 * IS_NULL means "no marker was written for this slot" — drop it. If the
 * resulting array is empty, collapse the mask zval to NULL so callers see it
 * as "no mask at all". */
static void dc_mask_cleanup(zval *mask)
{
	if (Z_TYPE_P(mask) != IS_ARRAY) {
		return;
	}
	SEPARATE_ARRAY(mask);
	HashTable *mht = Z_ARRVAL_P(mask);
	zend_hash_apply(mht, dc_mask_cleanup_apply);

	if (zend_hash_num_elements(mht) == 0) {
		zval_ptr_dtor(mask);
		ZVAL_NULL(mask);
	}
}

static void dc_copy_value(dc_ctx *ctx, zval *src, zval *dst, zval *mask_dst)
{
	/* Bail out early if we're about to overflow the C stack. Throws \Error
	 * via zend_call_stack_size_error(); callers propagate by checking
	 * EG(exception), which is how the rest of this file already handles
	 * errors. */
	if (UNEXPECTED(dc_check_stack_limit())) {
		return;
	}

	bool is_ref = 0;
	uint32_t ref_idx = 0;

	/* ── Reference detection (cold — refs are rare) ── */
	if (UNEXPECTED(Z_ISREF_P(src))) {
		zend_reference *ref = Z_REF_P(src);
		zval *inner = &ref->val;

		is_ref = 1;
		ctx->is_static = 0;

		/* Check if we've seen this reference before. Same raw-pointer key
		 * as dc_ref_add(). */
		zval *existing = zend_hash_index_find(&ctx->ref_map, (zend_ulong)(uintptr_t)ref);
		if (existing) {
			/* Re-encounter: emit -refId, set mask=false */
			uint32_t idx = (uint32_t)Z_LVAL_P(existing);
			ctx->refs[idx].count++;
			ZVAL_LONG(dst, -(zend_long)(idx + 1));
			DC_MASK_HARD_REF(mask_dst);
			return;
		}

		/* First encounter: register, then process the inner value.
		 * Store the index — the recursive walk may realloc ctx->refs. */
		dc_ref_add(ctx, ref, inner, inner);
		ref_idx = ctx->refs_count - 1;
		src = inner;
	}

	/* ── Scalar fast path (hot — most leaves are scalars) ──
	 * Catches IS_UNDEF/IS_NULL/IS_FALSE/IS_TRUE/IS_LONG/IS_DOUBLE/IS_STRING. */
	if (EXPECTED(Z_TYPE_P(src) <= IS_STRING)) {
		ZVAL_COPY(dst, src);
		goto handle_value;
	}

	/* ── Array ──────────────────────────────────── */
	if (Z_TYPE_P(src) == IS_ARRAY) {
		HashTable *src_ht = Z_ARRVAL_P(src);
		uint32_t n = zend_hash_num_elements(src_ht);
		if (n == 0
		 || (GC_FLAGS(src_ht) & GC_IMMUTABLE)
		 || dc_array_is_static(src_ht)) {
			/* Static array — just COW-share */
			ZVAL_COPY(dst, src);
			goto handle_value;
		}
		/* Recurse — dc_copy_array writes mask_dst directly (lazy init) */
		dc_copy_array(ctx, src_ht, dst, mask_dst);
		goto handle_value;
	}

	/* ── Resource (cold — rejected to match PHP DeepCloner) ── */
	if (UNEXPECTED(Z_TYPE_P(src) == IS_RESOURCE)) {
		zend_throw_exception_ex(dc_ce_not_instantiable_exception, 0,
			"Type \"%s resource\" is not instantiable.", zend_rsrc_list_get_rsrc_type(Z_RES_P(src)));
		return;
	}

	/* ── Enum (cold — passed by value) ── */
	if (UNEXPECTED(Z_OBJCE_P(src)->ce_flags & ZEND_ACC_ENUM)) {
		ZVAL_COPY(dst, src);
		goto handle_value;
	}

	/* ── Object ─────────────────────────────────── */
	ctx->is_static = 0;
	{
		uint32_t handle = Z_OBJ_HANDLE_P(src);
		/* Skip pool lookup for refcount==1 objects without __serialize */
		zval *pooled = (Z_REFCOUNT_P(src) == 1 && Z_OBJCE_P(src)->__serialize == NULL)
			? NULL
			: zend_hash_index_find(&ctx->object_pool, handle);
		if (UNEXPECTED(pooled != NULL)) {
			ctx->objects_count++;
			dc_pool_entry *entry = (dc_pool_entry *)Z_PTR_P(pooled);
			ZVAL_LONG(dst, entry->id);
			DC_MASK_OBJ_REF(mask_dst);
			goto handle_value;
		}
	}

	/* ── Named closure ──────────────────────────── */
	if (Z_OBJCE_P(src) == zend_ce_closure) {
		const zend_function *func = zend_get_closure_method_def(Z_OBJ_P(src));
		if (func && (func->common.fn_flags & ZEND_ACC_FAKE_CLOSURE)) {
			if (!dc_class_allowed(ctx->allowed_ht, zend_ce_closure->name)) {
				zend_value_error("deepclone_to_array(): class \"Closure\" is not allowed");
				return;
			}
			/* Build the encoded callable in dst */
			array_init_size(dst, 2);

			/* Element [0]: $this object, class name, or null */
			zval *this_ptr = zend_get_closure_this_ptr(src);
			zend_object *this_obj = (this_ptr && Z_TYPE_P(this_ptr) == IS_OBJECT) ? Z_OBJ_P(this_ptr) : NULL;

			zval undef;
			ZVAL_UNDEF(&undef);
			zval *slot0 = zend_hash_index_add_new(Z_ARRVAL_P(dst), 0, &undef);

			if (this_obj) {
				/* Recurse into the $this object so it gets pooled */
				zval this_zval;
				ZVAL_OBJ_COPY(&this_zval, this_obj);
				zval scratch_mask;
				ZVAL_UNDEF(&scratch_mask);
				dc_copy_value(ctx, &this_zval, slot0, &scratch_mask);
				zval_ptr_dtor(&this_zval);
				zval_ptr_dtor(&scratch_mask);
			} else {
				zend_class_entry *called_scope = func->common.scope;
				if (called_scope) {
					ZVAL_STR_COPY(slot0, called_scope->name);
				} else {
					ZVAL_NULL(slot0);
				}
			}

			/* Element [1]: method name */
			zval zname;
			ZVAL_STR_COPY(&zname, func->common.function_name);
			zend_hash_index_add_new(Z_ARRVAL_P(dst), 1, &zname);

			/* For non-public methods, wrap as [[callable], class, method].
			 * Note: closures wrapping private/protected methods carry BOTH
			 * ZEND_ACC_PUBLIC (synthetic, on the closure wrapper) AND
			 * ZEND_ACC_PRIVATE/PROTECTED (preserved from the original method).
			 * Check the original-method flags. */
			bool is_non_public = (func->common.fn_flags & (ZEND_ACC_PRIVATE | ZEND_ACC_PROTECTED)) != 0;
			if (is_non_public && func->common.scope) {
				zval inner_callable;
				ZVAL_COPY_VALUE(&inner_callable, dst);
				ZVAL_UNDEF(dst);
				array_init_size(dst, 3);
				zend_hash_index_add_new(Z_ARRVAL_P(dst), 0, &inner_callable);
				zval zclass, zmethod;
				ZVAL_STR_COPY(&zclass, func->common.scope->name);
				ZVAL_STR_COPY(&zmethod, func->common.function_name);
				zend_hash_index_add_new(Z_ARRVAL_P(dst), 1, &zclass);
				zend_hash_index_add_new(Z_ARRVAL_P(dst), 2, &zmethod);
			}

			DC_MASK_NAMED_CLOSURE(mask_dst);
			goto handle_value;
		}
		/* Anonymous closure — fall through to regular object handling */
	}

	/* ── Regular object processing ──────────────── */
	dc_process_object(ctx, src, dst, mask_dst);

handle_value:
	if (is_ref) {
		/* Save the processed value and its mask, then replace dst with
		 * -refId. Re-resolve from the saved index (ctx->refs may have
		 * been realloc'd by the recursive walk above). */
		dc_ref_entry *ref_entry = &ctx->refs[ref_idx];
		zval_ptr_dtor(&ref_entry->cur_value);
		ZVAL_COPY(&ref_entry->cur_value, dst);
		if (Z_TYPE_P(mask_dst) != IS_UNDEF && Z_TYPE_P(mask_dst) != IS_NULL) {
			zval_ptr_dtor(&ref_entry->cur_mask);
			ZVAL_COPY(&ref_entry->cur_mask, mask_dst);
		}
		ref_entry->tree_pos = dst;
		zval_ptr_dtor(dst);
		ZVAL_LONG(dst, -(zend_long)ref_entry->id);
		/* Override mask to the hard-ref marker */
		zval_ptr_dtor(mask_dst);
		DC_MASK_HARD_REF(mask_dst);
		ref_entry->mask_slot = mask_dst;
	}
}


/* ── Object processing ──────────────────────────────────────── */

/* Process an object value: pool it, walk its properties, write the resulting
 * pool ID to *dst and the marker to *mask_dst.
 *
 * - src is read-only — never mutated.
 * - dst receives the long pool id on success.
 * - mask_dst receives the marker (true for object refs).
 *
 * On failure (exception thrown), *dst and *mask_dst are left untouched. */
static void dc_process_object(dc_ctx *ctx, zval *src, zval *dst, zval *mask_dst)
{
	zend_object *obj = Z_OBJ_P(src);
	zend_class_entry *ce = obj->ce;
	uint32_t handle = Z_OBJ_HANDLE_P(src);
	HashTable *array_value = NULL;
	zval props_zval, retval;
	bool has_unserialize, need_release_array_value = false;
	HashTable *sleep_set = NULL;

	/* Reject disallowed classes before any allocation. */
	if (!dc_class_allowed(ctx->allowed_ht, ce->name)) {
		zend_value_error("deepclone_to_array(): class \"%s\" is not allowed", ZSTR_VAL(ce->name));
		return;
	}

	/* Allocate pool entry */
	dc_pool_entry *entry = emalloc(sizeof(dc_pool_entry));
	entry->id = ctx->next_obj_id++;
	entry->cidx = UINT32_MAX;
	entry->class_name = ce->name;
	entry->class_name_owned = false;
	entry->wakeup = 0;
	entry->props = NULL;
	entry->prop_mask = NULL;

	/* Register in id-indexed entries array — 1.5× growth, safe_erealloc
	 * for overflow detection (see the comment in dc_ref_add). */
	if (entry->id >= ctx->entries_cap) {
		ctx->entries_cap = ctx->entries_cap < 8 ? 8 : ((ctx->entries_cap * 3) >> 1) + 1;
		ctx->entries = safe_erealloc(ctx->entries, ctx->entries_cap, sizeof(dc_pool_entry *), 0);
	}
	ctx->entries[entry->id] = entry;

	/* Register in pool (sentinel) */
	zval zentry;
	ZVAL_PTR(&zentry, entry);
	zend_hash_index_add_new(&ctx->object_pool, handle, &zentry);

	array_init(&props_zval);

	/* ── stdClass fast path ─────────────────────── */
	if (ce == zend_standard_class_def) {
		HashTable *ht = obj->properties;
		if (ht && zend_hash_num_elements(ht) > 0) {
			zval scope_arr;
			ZVAL_ARR(&scope_arr, zend_array_dup(ht));
			zend_hash_add_new(Z_ARRVAL(props_zval), ce->name, &scope_arr);
		}
		has_unserialize = false;
		goto prepare_props;
	}

	uint8_t ci = dc_get_class_info(ctx, ce);
	has_unserialize = (ci & DC_CI_HAS_UNSERIALIZE) != 0;

	/* ── Reject non-instantiable classes (Reflection*, *IteratorIterator) ── */
	if (UNEXPECTED(ci & DC_CI_NOT_INSTANTIABLE)) {
		zend_throw_exception_ex(dc_ce_not_instantiable_exception, 0,
			"Type \"%s\" is not instantiable.", ZSTR_VAL(ce->name));
		zval_ptr_dtor(&props_zval);
		return;
	}

	/* ── __serialize ────────────────────────────── */
	if (ci & DC_CI_HAS_SERIALIZE) {
		if (UNEXPECTED(!(ci & DC_CI_SERIALIZE_PUBLIC))) {
			zend_throw_error(NULL, "Call to non-public method %s::__serialize()", ZSTR_VAL(ce->name));
			zval_ptr_dtor(&props_zval);
			return;
		}
		zend_call_method_with_0_params(obj, ce, &ce->__serialize, "__serialize", &retval);
		if (UNEXPECTED(EG(exception))) {
			zval_ptr_dtor(&props_zval);
			return;
		}
		if (UNEXPECTED(Z_TYPE(retval) != IS_ARRAY)) {
			zend_type_error("%s::__serialize() must return an array", ZSTR_VAL(ce->name));
			zval_ptr_dtor(&retval);
			zval_ptr_dtor(&props_zval);
			return;
		}
		if (has_unserialize) {
			zval_ptr_dtor(&props_zval);
			ZVAL_COPY_VALUE(&props_zval, &retval);
			goto prepare_props;
		}
		array_value = Z_ARRVAL(retval);
		need_release_array_value = true; /* retval owns the array */
		goto build_scoped_props;
	}

	/* ── Serializable / __PHP_Incomplete_Class (cold) ──── */
	if (UNEXPECTED(ce->serialize != NULL || ce == php_ce_incomplete_class)) {
		smart_str buf = {0};
		php_serialize_data_t var_hash;
		PHP_VAR_SERIALIZE_INIT(var_hash);
		php_var_serialize(&buf, src, &var_hash);
		PHP_VAR_SERIALIZE_DESTROY(var_hash);

		entry->class_name = smart_str_extract(&buf);
		entry->class_name_owned = true;
		entry->cidx = dc_class_index(ctx, entry->class_name);
		entry->props = NULL;
		entry->prop_mask = NULL;
		zval_ptr_dtor(&props_zval);
		ctx->objects_count++;
		goto replace_with_id;
	}

	/* ── __sleep filtering (cold) ──────────────────────── */
	if (UNEXPECTED(ci & DC_CI_HAS_SLEEP)) {
		zend_function *sleep_fn = zend_hash_find_ptr(&ce->function_table, ZSTR_KNOWN(ZEND_STR_SLEEP));
		zend_call_method_with_0_params(obj, ce, &sleep_fn, "__sleep", &retval);
		if (UNEXPECTED(EG(exception))) {
			zval_ptr_dtor(&props_zval);
			return;
		}
		if (UNEXPECTED(Z_TYPE(retval) != IS_ARRAY)) {
			php_error_docref(NULL, E_NOTICE,
				"serialize(): __sleep should return an array only containing the names of instance-variables to serialize");
			zval_ptr_dtor(&retval);
			/* Roll back the pool entry and write null into the parent slot */
			zval_ptr_dtor(&props_zval);
			ctx->entries[entry->id] = NULL;
			efree(entry);
			zend_hash_index_del(&ctx->object_pool, handle);
			ctx->next_obj_id--;
			ZVAL_NULL(dst);
			return;
		}
		/* Build sleep_set: name => 1 */
		ALLOC_HASHTABLE(sleep_set);
		zend_hash_init(sleep_set, zend_hash_num_elements(Z_ARRVAL(retval)), NULL, NULL, 0);
		zval *sleep_name;
		ZEND_HASH_FOREACH_VAL(Z_ARRVAL(retval), sleep_name) {
			if (Z_TYPE_P(sleep_name) == IS_STRING) {
				zval one;
				ZVAL_LONG(&one, 1);
				zend_hash_add(sleep_set, Z_STR_P(sleep_name), &one);
			}
		} ZEND_HASH_FOREACH_END();
		zval_ptr_dtor(&retval);
	}

	/* ── Get properties ─────────────────────────── */
	/*
	 * Fast path: walk properties_info_table directly (like serialize does).
	 * This avoids rebuilding the properties HashTable via zend_get_properties_for.
	 * Conditions: no custom handlers, no dynamic properties yet, not lazy.
	 */
	if (ce->type == ZEND_USER_CLASS
	 && obj->properties == NULL
	 && Z_OBJ_HT_P(src)->get_properties_for == NULL
	 && Z_OBJ_HT_P(src)->get_properties == zend_std_get_properties
	 && !zend_object_is_lazy(obj)) {
		/* Direct property slot access — compare with prototype slots */
		zend_property_info *prop_info;
		zval *prop;
		/* Default-value comparison: use default_properties_table for user classes.
		 * For internal classes (Error, etc.), skip — their constructors modify
		 * defaults so the table doesn't reflect runtime state. */
		zval *default_props = (ce->type == ZEND_USER_CLASS) ? ce->default_properties_table : NULL;

		for (uint32_t i = 0; i < ce->default_properties_count; i++) {
			prop_info = ce->properties_info_table[i];
			if (!prop_info || (prop_info->flags & ZEND_ACC_STATIC)) {
				continue;
			}
			prop = OBJ_PROP(obj, prop_info->offset);
			if (Z_TYPE_P(prop) == IS_UNDEF) {
				continue;
			}
			/* Unwrap references with refcount == 1 */
			if (Z_ISREF_P(prop) && Z_REFCOUNT_P(prop) == 1) {
				prop = Z_REFVAL_P(prop);
			}

			/* Unmangle prop_info->name for non-public properties */
			const char *unmangled_name;
			size_t unmangled_len;
			zend_string *prop_name;
			zend_string *scope_name;

			if (prop_info->flags & ZEND_ACC_PUBLIC) {
				prop_name = prop_info->name;
				scope_name = !(prop_info->flags & (ZEND_ACC_PROTECTED_SET | ZEND_ACC_PRIVATE_SET))
					? ZEND_STANDARD_CLASS_DEF_PTR->name : prop_info->ce->name;
			} else {
				const char *class_name_unused;
				zend_unmangle_property_name_ex(prop_info->name, &class_name_unused, &unmangled_name, &unmangled_len);
				/* Try to get an existing interned string (free for common names) */
				prop_name = zend_string_init_existing_interned(unmangled_name, unmangled_len, 0);
				scope_name = prop_info->ce->name;
			}

			/* __sleep filtering: check both mangled (prop_info->name) and unmangled name */
			if (sleep_set) {
				if (!zend_hash_exists(sleep_set, prop_info->name)
				 && !zend_hash_exists(sleep_set, prop_name)) {
					if (!(prop_info->flags & ZEND_ACC_PUBLIC)) {
						zend_string_release(prop_name);
					}
					continue;
				}
				zend_hash_del(sleep_set, prop_info->name);
				zend_hash_del(sleep_set, prop_name);
			}

			/* Skip default values — compare with default_properties_table */
			if (default_props) {
				uint32_t prop_num = OBJ_PROP_TO_NUM(prop_info->offset);
				zval *default_val = &default_props[prop_num];
				if (Z_TYPE_P(default_val) != IS_UNDEF && zend_is_identical(prop, default_val)) {
					/* Always keep trace properties */
					bool is_trace = zend_string_equals(prop_name, dc_str_trace)
						&& (instanceof_function(ce, zend_ce_exception) || instanceof_function(ce, zend_ce_error));
					if (!is_trace) {
						if (!(prop_info->flags & ZEND_ACC_PUBLIC)) {
							zend_string_release(prop_name);
						}
						continue;
					}
				}
			}

			/* Add to props_zval[scope][name] = value (COW).
			 * scope_name is always interned (class entry name). */
			zval *scope_ht = zend_hash_find_known_hash(Z_ARRVAL(props_zval), scope_name);
			if (!scope_ht) {
				zval new_ht;
				array_init(&new_ht);
				scope_ht = zend_hash_add_new(Z_ARRVAL(props_zval), scope_name, &new_ht);
			}
			Z_TRY_ADDREF_P(prop);
			zend_hash_add(Z_ARRVAL_P(scope_ht), prop_name, prop);
			if (!(prop_info->flags & ZEND_ACC_PUBLIC)) {
				zend_string_release(prop_name);
			}
		}

		/* For __unserialize objects, discard scoped props and use raw (array) cast */
		if (has_unserialize) {
			zval_ptr_dtor(&props_zval);
			/* Rebuild the raw (array) cast from property slots */
			array_init(&props_zval);
			for (uint32_t j = 0; j < ce->default_properties_count; j++) {
				zend_property_info *pj = ce->properties_info_table[j];
				if (!pj || (pj->flags & ZEND_ACC_STATIC)) continue;
				zval *pv = OBJ_PROP(obj, pj->offset);
				if (Z_TYPE_P(pv) == IS_UNDEF) continue;
				if (Z_ISREF_P(pv) && Z_REFCOUNT_P(pv) == 1) pv = Z_REFVAL_P(pv);
				/* Use unmangled name as key (matching (array) cast for public) */
				if (pj->flags & ZEND_ACC_PUBLIC) {
					Z_TRY_ADDREF_P(pv);
					zend_hash_add(Z_ARRVAL(props_zval), pj->name, pv);
				} else {
					/* Private/protected: use mangled key like (array) cast */
					Z_TRY_ADDREF_P(pv);
					zend_hash_add(Z_ARRVAL(props_zval), pj->name, pv);
				}
			}
		}

		goto done_props;
	}

	/* Fallback: (array) cast for objects with custom handlers */
	{
		HashTable *ht = zend_get_properties_for(src, ZEND_PROP_PURPOSE_ARRAY_CAST);
		if (ht) {
			array_value = ht;
			need_release_array_value = true;
		}
	}

	/* __unserialize without __serialize: use raw (array) cast as state props */
	if (has_unserialize && array_value) {
		zval_ptr_dtor(&props_zval);
		ZVAL_ARR(&props_zval, zend_array_dup(array_value));
		if (need_release_array_value) {
			zend_release_properties(array_value);
		}
		goto done_props;
	}

build_scoped_props:
	if (array_value) {
		HashTable *scope_map = dc_get_scope_map(ctx, ce);
		HashTable *proto = dc_get_proto(ctx, ce);
		zend_string *arr_key;
		zval *arr_val;

		ZEND_HASH_FOREACH_STR_KEY_VAL(array_value, arr_key, arr_val) {
			const char *key;
			size_t key_len;
			zend_string *prop_name = NULL;
			zend_string *scope_name = NULL;
			bool prop_name_owned = false;

			/* Dereference IS_INDIRECT (declared properties) and IS_REFERENCE */
			if (Z_TYPE_P(arr_val) == IS_INDIRECT) {
				arr_val = Z_INDIRECT_P(arr_val);
			}
			if (Z_ISREF_P(arr_val)) {
				arr_val = Z_REFVAL_P(arr_val);
			}

			if (!arr_key) {
				continue;
			}
			key = ZSTR_VAL(arr_key);
			key_len = ZSTR_LEN(arr_key);

			if (key_len == 0 || key[0] != '\0') {
				/* Public property */
				prop_name = arr_key;
				zval *scope_zv = zend_hash_find(scope_map, arr_key);
				scope_name = scope_zv ? Z_STR_P(scope_zv) : ZEND_STANDARD_CLASS_DEF_PTR->name;
			} else if (key[1] == '*') {
				/* Protected: \0*\0name */
				prop_name = zend_string_init_existing_interned(key + 3, key_len - 3, 0);
				prop_name_owned = true;
				zval *scope_zv = zend_hash_find(scope_map, prop_name);
				if (scope_zv) {
					scope_name = Z_STR_P(scope_zv);
				} else {
					zend_property_info *pi = zend_hash_find_ptr(&ce->properties_info, prop_name);
					scope_name = pi ? pi->ce->name : ce->name;
				}
			} else {
				/* Private: \0ClassName\0name */
				const char *sep = memchr(key + 2, '\0', key_len - 2);
				if (!sep) {
					continue;
				}
				size_t class_len = sep - key - 1;
				scope_name = zend_string_init_existing_interned(key + 1, class_len, 0);
				prop_name = zend_string_init_existing_interned(sep + 1, key_len - class_len - 2, 0);
				prop_name_owned = true;
			}

			/* __sleep filtering: match by mangled key first, then by unmangled name
			 * (but only if not an inherited private property — same as PHP Exporter) */
			if (sleep_set) {
				bool found = zend_hash_exists(sleep_set, arr_key);
				if (!found) {
					/* For private props of parent classes, unmangled name must not match */
					bool is_inherited_private = (key[0] == '\0' && key[1] != '*');
					if (is_inherited_private) {
						const char *sep = memchr(key + 2, '\0', key_len - 2);
						if (sep) {
							size_t class_len = sep - key - 1;
							zend_string *prop_class = zend_string_init_existing_interned(key + 1, class_len, 0);
							is_inherited_private = !zend_string_equals(prop_class, ce->name);
							zend_string_release(prop_class);
						}
					}
					if (!is_inherited_private) {
						found = zend_hash_exists(sleep_set, prop_name);
					}
				}
				if (found) {
					zend_hash_del(sleep_set, arr_key);
					zend_hash_del(sleep_set, prop_name);
				} else {
					goto next_prop;
				}
			}

			/* Skip default values, except for the call-context-sensitive Throwable
			 * properties (file, line, trace). For those, the lazily-built prototype
			 * inherits the same file/line as the actual exception, so the proto
			 * comparison would spuriously drop them — always emit them instead. */
			if (!zend_string_equals(arr_key, dc_str_file_mangled)
			 && !zend_string_equals(arr_key, dc_str_line_mangled)
			 && !zend_string_equals(arr_key, dc_str_error_trace_mangled)
			 && !zend_string_equals(arr_key, dc_str_exception_trace_mangled)) {
				zval *proto_val = zend_hash_find(proto, arr_key);
				if (proto_val && zend_is_identical(arr_val, proto_val)) {
					goto next_prop;
				}
			}

			zend_string_addref(scope_name);

			/* Add to scoped properties */
			{
				zval *scope_ht = zend_hash_find(Z_ARRVAL(props_zval), scope_name);
				if (!scope_ht) {
					zval new_ht;
					array_init(&new_ht);
					scope_ht = zend_hash_add_new(Z_ARRVAL(props_zval), scope_name, &new_ht);
				}
				Z_TRY_ADDREF_P(arr_val);
				zend_hash_add_new(Z_ARRVAL_P(scope_ht), prop_name, arr_val);
			}
			zend_string_release(scope_name);

next_prop:
			if (prop_name_owned) {
				zend_string_release(prop_name);
			}
		} ZEND_HASH_FOREACH_END();

		if (need_release_array_value) {
			zend_release_properties(array_value);
		}
	}

done_props:
	/* __sleep: warn about non-existent members */
	if (sleep_set) {
		zend_string *missing;
		ZEND_HASH_FOREACH_STR_KEY(sleep_set, missing) {
			if (missing && !zend_hash_find_known_hash(&ce->properties_info, missing)) {
				php_error_docref(NULL, E_NOTICE,
					"serialize(): \"%s\" returned as member variable from __sleep() but does not exist",
					ZSTR_VAL(missing));
			}
		} ZEND_HASH_FOREACH_END();
		zend_hash_destroy(sleep_set);
		FREE_HASHTABLE(sleep_set);
	}

prepare_props:
	/* Compute and cache the deduped class index */
	entry->cidx = dc_class_index(ctx, entry->class_name);

	if (has_unserialize) {
		/* For __unserialize objects: prepare a flat array as the state argument */
		zval prepared_zval, prop_mask;
		ZVAL_UNDEF(&prepared_zval);
		ZVAL_UNDEF(&prop_mask);
		dc_copy_array(ctx, Z_ARRVAL(props_zval), &prepared_zval, &prop_mask);
		zval_ptr_dtor(&props_zval);
		ZVAL_COPY_VALUE(&props_zval, &prepared_zval);
		dc_mask_cleanup(&prop_mask);

		entry->props = Z_ARRVAL(props_zval);
		entry->prop_mask = (Z_TYPE(prop_mask) == IS_ARRAY) ? Z_ARRVAL(prop_mask) : NULL;
	} else {
		/* For normal objects: transpose directly into ctx->properties[scope][name][id]
		 * and ctx->resolve[scope][name][id] during the walk.
		 *
		 * The recursive dc_copy_value() call may grow the same hash tables
		 * we are inserting into, so we never cache bucket pointers across
		 * the call: write into a stack-local temp, then re-find and insert
		 * after the call returns. */
		uint32_t entry_id = entry->id;

		zend_string *scope;
		zval *scope_vals;
		ZEND_HASH_FOREACH_STR_KEY_VAL(Z_ARRVAL(props_zval), scope, scope_vals) {
			zend_string *name;
			zval *raw_val;
			ZEND_HASH_FOREACH_STR_KEY_VAL(Z_ARRVAL_P(scope_vals), name, raw_val) {
				if (Z_TYPE(ctx->properties) == IS_UNDEF) {
					array_init_size(&ctx->properties, 1);
				}
				/* scope is a class name (interned); name's hash is populated
				 * from the HashTable iteration above. */
				zval *out_scope = zend_hash_find_known_hash(Z_ARRVAL(ctx->properties), scope);
				if (!out_scope) {
					zval new_ht;
					array_init_size(&new_ht, 4);
					out_scope = zend_hash_add_new(Z_ARRVAL(ctx->properties), scope, &new_ht);
				}
				zval *out_name = zend_hash_find_known_hash(Z_ARRVAL_P(out_scope), name);
				if (!out_name) {
					zval new_ht;
					array_init_size(&new_ht, 1);
					out_name = zend_hash_add_new(Z_ARRVAL_P(out_scope), name, &new_ht);
				}

				/* Fast path: scalar values can't mutate ctx, so no placeholder,
				 * no recursion, no re-lookup. This covers IS_UNDEF/IS_NULL/
				 * IS_FALSE/IS_TRUE/IS_LONG/IS_DOUBLE/IS_STRING (not IS_REFERENCE). */
				if (EXPECTED(Z_TYPE_P(raw_val) <= IS_STRING)) {
					zval copy;
					ZVAL_COPY(&copy, raw_val);
					zend_hash_index_add_new(Z_ARRVAL_P(out_name), entry_id, &copy);
					continue;
				}

				/* Reserve a placeholder (IS_NULL, not IS_UNDEF — packed arrays
				 * treat UNDEF as a tombstone in zend_hash_index_find). */
				zval null_ph;
				ZVAL_NULL(&null_ph);
				zend_hash_index_add_new(Z_ARRVAL_P(out_name), entry_id, &null_ph);

				/* Recurse into a stack-local temp, then re-find and insert. */
				zval temp_dst;
				ZVAL_UNDEF(&temp_dst);
				zval mask_slot_zv;
				ZVAL_UNDEF(&mask_slot_zv);
				dc_copy_value(ctx, raw_val, &temp_dst, &mask_slot_zv);
				if (UNEXPECTED(EG(exception))) {
					zval_ptr_dtor(&temp_dst);
					zval_ptr_dtor(&mask_slot_zv);
					zval_ptr_dtor(&props_zval);
					return;
				}
				dc_mask_cleanup(&mask_slot_zv);

				out_scope = zend_hash_find_known_hash(Z_ARRVAL(ctx->properties), scope);
				out_name = zend_hash_find_known_hash(Z_ARRVAL_P(out_scope), name);
				zval *dst_slot = zend_hash_index_find(Z_ARRVAL_P(out_name), entry_id);
				ZVAL_COPY_VALUE(dst_slot, &temp_dst);

				if (Z_TYPE(mask_slot_zv) != IS_UNDEF && Z_TYPE(mask_slot_zv) != IS_NULL) {
					if (Z_TYPE(ctx->resolve) == IS_UNDEF) {
						array_init_size(&ctx->resolve, 1);
					}
					zval *out_rscope = zend_hash_find_known_hash(Z_ARRVAL(ctx->resolve), scope);
					if (!out_rscope) {
						zval new_ht;
						array_init_size(&new_ht, 1);
						out_rscope = zend_hash_add_new(Z_ARRVAL(ctx->resolve), scope, &new_ht);
					}
					zval *out_rname = zend_hash_find_known_hash(Z_ARRVAL_P(out_rscope), name);
					if (!out_rname) {
						zval new_ht;
						array_init_size(&new_ht, 1);
						out_rname = zend_hash_add_new(Z_ARRVAL_P(out_rscope), name, &new_ht);
					}
					zend_hash_index_add_new(Z_ARRVAL_P(out_rname), entry_id, &mask_slot_zv);
				}
			} ZEND_HASH_FOREACH_END();
		} ZEND_HASH_FOREACH_END();

		zval_ptr_dtor(&props_zval);
		entry->props = NULL;
		entry->prop_mask = NULL;
	}

	/* Increment objects_count and assign wakeup AFTER recursion so children
	 * (which were processed during the recursive walk) get smaller wakeup IDs */
	ctx->objects_count++;
	if (has_unserialize) {
		entry->wakeup = -(int) ctx->objects_count;
	} else if (ce != zend_standard_class_def && (dc_get_class_info(ctx, ce) & DC_CI_HAS_WAKEUP)) {
		entry->wakeup = (int) ctx->objects_count;
	}

replace_with_id:
	/* Write the pool ID and the object marker into the caller's slots. */
	ZVAL_LONG(dst, entry->id);
	DC_MASK_OBJ_REF(mask_dst);
}

static int dc_compare_bucket_keys(Bucket *a, Bucket *b) {
	if (a->h < b->h) return -1;
	if (a->h > b->h) return 1;
	return 0;
}

/* ── Build final output array ───────────────────────────────── */

static void dc_build_output(dc_ctx *ctx, zval *prepared, zval *top_mask, zval *return_value)
{
	array_init(return_value);

	/* Check if mask is non-empty (could be scalar, array, or UNDEF) */
	bool has_mask;
	if (Z_TYPE_P(top_mask) == IS_UNDEF || Z_TYPE_P(top_mask) == IS_NULL) {
		has_mask = false;
	} else if (Z_TYPE_P(top_mask) == IS_ARRAY) {
		has_mask = zend_hash_num_elements(Z_ARRVAL_P(top_mask)) > 0;
	} else {
		has_mask = true;
	}

	/* Count shared refs (count > 0) */
	uint32_t shared_refs = 0;
	for (uint32_t i = 0; i < ctx->refs_count; i++) {
		if (ctx->refs[i].count > 0) shared_refs++;
	}

	/* If no objects and no shared refs and no mask — static value */
	if (ctx->next_obj_id == 0 && shared_refs == 0 && !has_mask) {
		zend_hash_add(Z_ARRVAL_P(return_value), dc_key_value, prepared);
		Z_TRY_ADDREF_P(prepared);
		return;
	}

	/* properties, resolve, classes are already built by dc_process_object.
	 * We just need to assemble objectMeta and states from the entries. */
	uint32_t n_obj = ctx->next_obj_id;
	zval object_meta, states;
	array_init_size(&object_meta, n_obj);
	array_init_size(&states, 0);

	/* Iterate entries in ID order to build objectMeta and states */
	for (uint32_t id = 0; id < n_obj; id++) {
		dc_pool_entry *e = ctx->entries[id];
		uint32_t cidx = e->cidx;

		/* objectMeta[id] = wakeup ? [cidx, wakeup] : cidx */
		if (e->wakeup != 0) {
			zval meta;
			array_init_size(&meta, 2);
			zval zc, zw;
			ZVAL_LONG(&zc, cidx);
			ZVAL_LONG(&zw, e->wakeup);
			zend_hash_index_add_new(Z_ARRVAL(meta), 0, &zc);
			zend_hash_index_add_new(Z_ARRVAL(meta), 1, &zw);
			zend_hash_index_add_new(Z_ARRVAL(object_meta), id, &meta);
		} else {
			zval zc;
			ZVAL_LONG(&zc, cidx);
			zend_hash_index_add_new(Z_ARRVAL(object_meta), id, &zc);
		}

		/* States */
		if (e->wakeup > 0) {
			zval zid;
			ZVAL_LONG(&zid, id);
			zend_hash_index_add_new(Z_ARRVAL(states), (zend_ulong)e->wakeup, &zid);
		} else if (e->wakeup < 0) {
			/* Transfer ownership of props/prop_mask to the states array. */
			zval state_entry;
			if (e->prop_mask) {
				array_init_size(&state_entry, 3);
				zval zid, zprops, zmask;
				ZVAL_LONG(&zid, id);
				ZVAL_ARR(&zprops, e->props);
				ZVAL_ARR(&zmask, e->prop_mask);
				zend_hash_index_add_new(Z_ARRVAL(state_entry), 0, &zid);
				zend_hash_index_add_new(Z_ARRVAL(state_entry), 1, &zprops);
				zend_hash_index_add_new(Z_ARRVAL(state_entry), 2, &zmask);
			} else {
				array_init_size(&state_entry, 2);
				zval zid, zprops;
				ZVAL_LONG(&zid, id);
				ZVAL_ARR(&zprops, e->props);
				zend_hash_index_add_new(Z_ARRVAL(state_entry), 0, &zid);
				zend_hash_index_add_new(Z_ARRVAL(state_entry), 1, &zprops);
			}
			zend_hash_index_add_new(Z_ARRVAL(states), (zend_ulong) -e->wakeup, &state_entry);
			e->props = NULL;      /* Owned by states now */
			e->prop_mask = NULL;  /* Owned by states now */
		}
	}

	/* Sort states by key */
	zend_hash_sort(Z_ARRVAL(states), dc_compare_bucket_keys, 0);

	/* ── Build refs and refMasks ────────────────── */
	zval refs_out, ref_masks_out;
	array_init_size(&refs_out, shared_refs);
	array_init_size(&ref_masks_out, 0);

	for (uint32_t i = 0; i < ctx->refs_count; i++) {
		dc_ref_entry *re = &ctx->refs[i];
		if (re->count == 0) continue; /* Unshared ref — was unwrapped */

		uint32_t ref_id = re->id;
		zval *orig = &re->orig_type;
		zval *cur = &re->cur_value;

		if (Z_TYPE_P(orig) == IS_OBJECT && !(Z_OBJCE_P(orig)->ce_flags & ZEND_ACC_ENUM)) {
			/* Object ref */
			uint32_t handle = Z_OBJ_HANDLE_P(orig);
			zval *pooled = zend_hash_index_find(&ctx->object_pool, handle);
			if (pooled) {
				dc_pool_entry *pe = (dc_pool_entry *)Z_PTR_P(pooled);
				zval zid;
				ZVAL_LONG(&zid, pe->id);
				zend_hash_index_add_new(Z_ARRVAL(refs_out), ref_id, &zid);
				zval marker;
				ZVAL_TRUE(&marker);
				zend_hash_index_add_new(Z_ARRVAL(ref_masks_out), ref_id, &marker);
			} else {
				Z_TRY_ADDREF_P(cur);
				zend_hash_index_add_new(Z_ARRVAL(refs_out), ref_id, cur);
			}
		} else if (Z_TYPE_P(orig) == IS_OBJECT && (Z_OBJCE_P(orig)->ce_flags & ZEND_ACC_ENUM)) {
			/* UnitEnum ref — synthesize "Class::Case" once per occurrence.
			 * If the same combined string happens to be interned already
			 * (unlikely but free when it does happen), reuse the interned
			 * copy so repeated hard-refs to the same enum case end up
			 * sharing the same zend_string. */
			zend_string *case_name = Z_STR_P(zend_enum_fetch_case_name(Z_OBJ_P(orig)));
			zend_string *enum_str = zend_strpprintf(0, "%s::%s",
				ZSTR_VAL(Z_OBJCE_P(orig)->name),
				ZSTR_VAL(case_name));
			zend_string *interned = zend_string_init_existing_interned(
				ZSTR_VAL(enum_str), ZSTR_LEN(enum_str), 0);
			if (interned != enum_str) {
				zend_string_release(enum_str);
				enum_str = interned;
			}
			zval zenum;
			ZVAL_STR(&zenum, enum_str);
			zend_hash_index_add_new(Z_ARRVAL(refs_out), ref_id, &zenum);
			zval marker;
			ZVAL_INTERNED_STR(&marker, zend_string_init_interned("e", 1, 0));
			zend_hash_index_add_new(Z_ARRVAL(ref_masks_out), ref_id, &marker);
		} else {
			/* Scalar or array ref — use saved cur_value and cur_mask */
			Z_TRY_ADDREF_P(cur);
			zend_hash_index_add_new(Z_ARRVAL(refs_out), ref_id, cur);
			if (Z_TYPE(re->cur_mask) != IS_NULL) {
				zval mask_copy;
				ZVAL_COPY(&mask_copy, &re->cur_mask);
				zend_hash_index_add_new(Z_ARRVAL(ref_masks_out), ref_id, &mask_copy);
			}
		}
	}

	/* ── Assemble output ───────────────────────── */

	/* classes: string if 1, array if >1, '' if 0 (transfer ownership from ctx) */
	uint32_t num_classes = (Z_TYPE(ctx->classes) == IS_ARRAY)
		? zend_hash_num_elements(Z_ARRVAL(ctx->classes)) : 0;
	if (num_classes == 1) {
		zval *first = zend_hash_index_find(Z_ARRVAL(ctx->classes), 0);
		zval zc;
		ZVAL_COPY(&zc, first);
		zend_hash_add_new(Z_ARRVAL_P(return_value), dc_key_classes, &zc);
	} else if (num_classes == 0) {
		zval zc;
		ZVAL_EMPTY_STRING(&zc);
		zend_hash_add_new(Z_ARRVAL_P(return_value), dc_key_classes, &zc);
	} else {
		zval zc;
		ZVAL_COPY(&zc, &ctx->classes);
		zend_hash_add_new(Z_ARRVAL_P(return_value), dc_key_classes, &zc);
	}

	/* objectMeta: count if all same class with wakeup=0, else the array */
	{
		uint32_t n = zend_hash_num_elements(Z_ARRVAL(object_meta));
		bool all_zero = true;
		zval *v;
		ZEND_HASH_FOREACH_VAL(Z_ARRVAL(object_meta), v) {
			if (Z_TYPE_P(v) != IS_LONG || Z_LVAL_P(v) != 0) {
				all_zero = false;
				break;
			}
		} ZEND_HASH_FOREACH_END();

		if (all_zero) {
			zval zn;
			ZVAL_LONG(&zn, n);
			zend_hash_add_new(Z_ARRVAL_P(return_value), dc_key_object_meta, &zn);
			zval_ptr_dtor(&object_meta);
		} else {
			zend_hash_add_new(Z_ARRVAL_P(return_value), dc_key_object_meta, &object_meta);
		}
	}

	/* prepared */
	Z_TRY_ADDREF_P(prepared);
	zend_hash_add_new(Z_ARRVAL_P(return_value), dc_key_prepared, prepared);

	/* mask (if non-empty and prepared is not a plain integer) */
	if (Z_TYPE_P(prepared) != IS_LONG && has_mask) {
		Z_TRY_ADDREF_P(top_mask);
		zend_hash_add_new(Z_ARRVAL_P(return_value), dc_key_mask, top_mask);
	}

	/* properties (if non-empty) — transfer ownership from ctx */
	if (Z_TYPE(ctx->properties) == IS_ARRAY && zend_hash_num_elements(Z_ARRVAL(ctx->properties)) > 0) {
		zval p;
		ZVAL_COPY(&p, &ctx->properties);
		zend_hash_add_new(Z_ARRVAL_P(return_value), dc_key_properties, &p);
	}

	/* resolve (if non-empty) — transfer ownership from ctx */
	if (Z_TYPE(ctx->resolve) == IS_ARRAY && zend_hash_num_elements(Z_ARRVAL(ctx->resolve)) > 0) {
		zval r;
		ZVAL_COPY(&r, &ctx->resolve);
		zend_hash_add_new(Z_ARRVAL_P(return_value), dc_key_resolve, &r);
	}

	/* states (if non-empty) */
	if (zend_hash_num_elements(Z_ARRVAL(states)) > 0) {
		zend_hash_add_new(Z_ARRVAL_P(return_value), dc_key_states, &states);
	} else {
		zval_ptr_dtor(&states);
	}

	/* refs (if non-empty) */
	if (zend_hash_num_elements(Z_ARRVAL(refs_out)) > 0) {
		zend_hash_add_new(Z_ARRVAL_P(return_value), dc_key_refs, &refs_out);
	} else {
		zval_ptr_dtor(&refs_out);
	}

	/* refMasks (if non-empty) */
	if (zend_hash_num_elements(Z_ARRVAL(ref_masks_out)) > 0) {
		zend_hash_add_new(Z_ARRVAL_P(return_value), dc_key_ref_masks, &ref_masks_out);
	} else {
		zval_ptr_dtor(&ref_masks_out);
	}

	/* Pool entries whose props/prop_mask were moved into states above had those
	 * fields nulled — dc_ctx_destroy will free the rest. */
}

/* ── deepclone_to_array() — produce the pure-array format ──── */

PHP_FUNCTION(deepclone_to_array)
{
	zval *value;
	HashTable *allowed_ht = NULL;

	ZEND_PARSE_PARAMETERS_START(1, 2)
		Z_PARAM_ZVAL(value)
		Z_PARAM_OPTIONAL
		Z_PARAM_ARRAY_HT_OR_NULL(allowed_ht)
	ZEND_PARSE_PARAMETERS_END();

	/* Reject resources at the top level just like the walker does mid-tree.
	 * Without this, resource roots would hit the fast path below and get
	 * returned wrapped in ['value' => $resource], which is no longer a pure
	 * array and silently breaks downstream serializers. */
	if (UNEXPECTED(Z_TYPE_P(value) == IS_RESOURCE)) {
		zend_throw_exception_ex(dc_ce_not_instantiable_exception, 0,
			"%s resource", zend_rsrc_list_get_rsrc_type(Z_RES_P(value)));
		RETURN_THROWS();
	}

	/* Static values: return ['value' => $value] */
	if (Z_TYPE_P(value) != IS_OBJECT && (Z_TYPE_P(value) != IS_ARRAY || zend_hash_num_elements(Z_ARRVAL_P(value)) == 0)) {
		array_init(return_value);
		Z_TRY_ADDREF_P(value);
		zend_hash_add_new(Z_ARRVAL_P(return_value), dc_key_value, value);
		return;
	}
	if (Z_TYPE_P(value) == IS_OBJECT && (Z_OBJCE_P(value)->ce_flags & ZEND_ACC_ENUM)) {
		array_init(return_value);
		Z_TRY_ADDREF_P(value);
		zend_hash_add_new(Z_ARRVAL_P(return_value), dc_key_value, value);
		return;
	}

	dc_ctx ctx;
	dc_ctx_init(&ctx);
	if (allowed_ht) {
		ctx.allowed_ht = dc_build_allowed_set(allowed_ht, "deepclone_to_array");
		if (!ctx.allowed_ht) {
			dc_ctx_destroy(&ctx);
			return;
		}
	}

	zval prepared, top_mask;
	ZVAL_UNDEF(&prepared);
	ZVAL_UNDEF(&top_mask);

	/* Top-level walk: top_mask receives the mask directly */
	dc_copy_value(&ctx, value, &prepared, &top_mask);

	/* Fast-path abort: if the walker hit the stack limit, a non-instantiable
	 * class or a throwing __serialize, we have a partially-populated payload
	 * that the post-processing passes below would happily walk anyway. Bail
	 * out and let the caller see the exception as-is. */
	if (UNEXPECTED(EG(exception))) {
		zval_ptr_dtor(&prepared);
		zval_ptr_dtor(&top_mask);
		dc_ctx_destroy(&ctx);
		return;
	}

	/* Post-process: unwrap unshared refs (count=0) */
	for (uint32_t i = 0; i < ctx.refs_count; i++) {
		dc_ref_entry *re = &ctx.refs[i];
		if (re->count == 0 && re->tree_pos) {
			zval_ptr_dtor(re->tree_pos);
			ZVAL_COPY(re->tree_pos, &re->cur_value);
			/* Restore the mask slot from the saved cur_mask (or reset to NULL,
			 * which dc_mask_cleanup() will subsequently strip). */
			if (re->mask_slot) {
				zval_ptr_dtor(re->mask_slot);
				if (Z_TYPE(re->cur_mask) != IS_UNDEF) {
					ZVAL_COPY(re->mask_slot, &re->cur_mask);
				} else {
					ZVAL_NULL(re->mask_slot);
				}
			}
		}
	}

	/* Strip the NULL placeholders that dc_copy_array seeded and any slots that
	 * the unshared-ref unwrap pass cleared. */
	dc_mask_cleanup(&top_mask);

	/* Recompute is_static after unwrapping unshared refs:
	 * if no objects, no shared refs, no remaining mask, the value is static. */
	uint32_t shared_refs = 0;
	for (uint32_t i = 0; i < ctx.refs_count; i++) {
		if (ctx.refs[i].count > 0) shared_refs++;
	}
	bool effectively_static = (ctx.next_obj_id == 0 && shared_refs == 0
		&& (Z_TYPE(top_mask) == IS_UNDEF || Z_TYPE(top_mask) == IS_NULL
		    || (Z_TYPE(top_mask) == IS_ARRAY && zend_hash_num_elements(Z_ARRVAL(top_mask)) == 0)));

	if (effectively_static) {
		/* Static value — return ['value' => prepared] (which has any unshared
		 * refs already unwrapped). For pure scalars/arrays, prepared was just
		 * a COW copy of the input, so this is essentially free. */
		array_init(return_value);
		Z_TRY_ADDREF(prepared);
		zend_hash_add_new(Z_ARRVAL_P(return_value), dc_key_value, &prepared);
		zval_ptr_dtor(&prepared);
		zval_ptr_dtor(&top_mask);
		dc_ctx_destroy(&ctx);
		return;
	}

	dc_build_output(&ctx, &prepared, &top_mask, return_value);

	zval_ptr_dtor(&prepared);
	zval_ptr_dtor(&top_mask);
	dc_ctx_destroy(&ctx);
}

/* ── deepclone_from_array() — reconstruct the value graph ──── */

/*
 * Resolve a value using its mask marker. Writes the resolved value to *retval.
 * Throws \ValueError on malformed input — callers must check EG(exception)
 * after calling and bail out if set.
 *
 *   true   object reference     →  objects[value]    (value is the pool id)
 *   false  hard PHP &-reference →  &refs[-value]
 *   0      named closure        →  reconstruct callable from value
 *   'e'    UnitEnum             →  resolve "Class::Case" string
 *   array  nested mask          →  recurse into the array's elements
 *   other  no marker            →  copy value as-is
 */
static void dc_resolve(zval *value, zval *mask, zval *objects, uint32_t num_objects, HashTable *refs, zval *retval)
{
	if (EXPECTED(DC_MASK_IS_OBJ_REF(mask))) {
		if (UNEXPECTED(Z_TYPE_P(value) != IS_LONG)) {
			zend_value_error("deepclone_from_array(): malformed payload, object reference value must be of type int, %s given", zend_zval_value_name(value));
			return;
		}
		zend_long id = Z_LVAL_P(value);
		zval *target;
		if (EXPECTED(id >= 0)) {
			if (UNEXPECTED((zend_ulong) id >= num_objects)) {
				zend_value_error("deepclone_from_array(): malformed payload, unknown object id " ZEND_LONG_FMT, id);
				return;
			}
			target = &objects[id];
		} else {
			target = zend_hash_index_find(refs, -id);
			if (UNEXPECTED(!target)) {
				zend_value_error("deepclone_from_array(): malformed payload, unknown ref id " ZEND_LONG_FMT, -id);
				return;
			}
		}
		ZVAL_COPY(retval, target);
		return;
	}

	if (DC_MASK_IS_HARD_REF(mask)) {
		if (UNEXPECTED(Z_TYPE_P(value) != IS_LONG)) {
			zend_value_error("deepclone_from_array(): malformed payload, hard-ref value must be of type int, %s given", zend_zval_value_name(value));
			return;
		}
		/* Guard against ZEND_LONG_MIN — negating it is signed-overflow UB. */
		if (UNEXPECTED(Z_LVAL_P(value) <= ZEND_LONG_MIN || Z_LVAL_P(value) >= 0)) {
			zend_value_error("deepclone_from_array(): malformed payload, ref id out of range");
			return;
		}
		zend_long rid = -Z_LVAL_P(value);
		zval *ref_slot = zend_hash_index_find(refs, rid);
		if (UNEXPECTED(!ref_slot)) {
			zend_value_error("deepclone_from_array(): malformed payload, unknown ref id " ZEND_LONG_FMT, rid);
			return;
		}
		if (!Z_ISREF_P(ref_slot)) {
			ZVAL_MAKE_REF(ref_slot);
		}
		ZVAL_COPY(retval, ref_slot);
		return;
	}

	if (DC_MASK_IS_NAMED_CLOSURE(mask)) {
		/* Named closure: value is [obj_or_class, method] or [[callable], class, method] */
		if (Z_TYPE_P(value) != IS_ARRAY) {
			zend_value_error("deepclone_from_array(): malformed payload, named-closure value must be of type array, %s given", zend_zval_value_name(value));
			return;
		}
		zval *arr = value;
		zval *elem0 = zend_hash_index_find(Z_ARRVAL_P(arr), 0);
		zval *elem1 = zend_hash_index_find(Z_ARRVAL_P(arr), 1);
		if (!elem0 || !elem1) {
			zend_value_error("deepclone_from_array(): malformed payload, named-closure value must have at least 2 elements");
			return;
		}

		zval *callable_arr;
		bool is_private = false;
		zend_string *priv_class = NULL, *priv_method = NULL;

		if (Z_TYPE_P(elem0) == IS_ARRAY) {
			/* Private method: [[obj, name], class, method] */
			callable_arr = elem0;
			is_private = true;
			if (Z_TYPE_P(elem1) != IS_STRING) {
				zend_value_error("deepclone_from_array(): malformed payload, named-closure private class name must be of type string, %s given", zend_zval_value_name(elem1));
				return;
			}
			priv_class = Z_STR_P(elem1);
			zval *elem2 = zend_hash_index_find(Z_ARRVAL_P(arr), 2);
			if (!elem2 || Z_TYPE_P(elem2) != IS_STRING) {
				zend_value_error("deepclone_from_array(): malformed payload, named-closure private method name must be of type string");
				return;
			}
			priv_method = Z_STR_P(elem2);
		} else {
			callable_arr = arr;
		}

		zval *zobj = zend_hash_index_find(Z_ARRVAL_P(callable_arr), 0);
		zval *zname = zend_hash_index_find(Z_ARRVAL_P(callable_arr), 1);
		if (!zobj || !zname || Z_TYPE_P(zname) != IS_STRING) {
			zend_value_error("deepclone_from_array(): malformed payload, named-closure callable must be [obj_or_class_or_null, string]");
			return;
		}
		zval resolved_obj;

		if (Z_TYPE_P(zobj) == IS_LONG) {
			zend_long id = Z_LVAL_P(zobj);
			zval *target;
			if (id >= 0) {
				if ((zend_ulong) id >= num_objects) {
					zend_value_error("deepclone_from_array(): malformed payload, named-closure references unknown id " ZEND_LONG_FMT, id);
					return;
				}
				target = &objects[id];
			} else {
				target = zend_hash_index_find(refs, -id);
				if (!target) {
					zend_value_error("deepclone_from_array(): malformed payload, named-closure references unknown id " ZEND_LONG_FMT, id);
					return;
				}
			}
			ZVAL_COPY(&resolved_obj, target);
		} else {
			ZVAL_COPY(&resolved_obj, zobj);
		}

		if (is_private) {
			zend_class_entry *ce = zend_lookup_class(priv_class);
			if (ce) {
				zend_function *func = zend_hash_find_ptr_lc(&ce->function_table, priv_method);
				if (func) {
					zval *this_ptr = (Z_TYPE(resolved_obj) == IS_OBJECT) ? &resolved_obj : NULL;
					zend_create_fake_closure(retval, func, ce, ce, this_ptr);
				}
			}
		} else {
			zend_string *name = Z_STR_P(zname);

			if (Z_TYPE(resolved_obj) == IS_NULL) {
				zend_function *func = zend_hash_find_ptr_lc(CG(function_table), name);
				if (func) {
					zend_create_fake_closure(retval, func, NULL, NULL, NULL);
				}
			} else if (Z_TYPE(resolved_obj) == IS_OBJECT) {
				zend_class_entry *ce = Z_OBJCE(resolved_obj);
				zend_function *func = zend_hash_find_ptr_lc(&ce->function_table, name);
				if (func) {
					zend_create_fake_closure(retval, func, ce, ce, &resolved_obj);
				}
			} else if (Z_TYPE(resolved_obj) == IS_STRING) {
				zend_class_entry *ce = zend_lookup_class(Z_STR(resolved_obj));
				if (ce) {
					zend_function *func = zend_hash_find_ptr_lc(&ce->function_table, name);
					if (func) {
						zend_create_fake_closure(retval, func, ce, ce, NULL);
					}
				}
			}
		}
		if (Z_ISUNDEF_P(retval)) {
			zval_ptr_dtor(&resolved_obj);
			zend_value_error("deepclone_from_array(): malformed payload, named-closure function or method not found");
			return;
		}
		zval_ptr_dtor(&resolved_obj);
		return;
	}

	if (Z_TYPE_P(mask) == IS_STRING && ZSTR_LEN(Z_STR_P(mask)) == 1 && ZSTR_VAL(Z_STR_P(mask))[0] == 'e') {
		/* UnitEnum: parse "Class::Case", resolve via zend_enum_get_case */
		if (Z_TYPE_P(value) != IS_STRING) {
			zend_value_error("deepclone_from_array(): malformed payload, enum value must be of type string, %s given", zend_zval_value_name(value));
			return;
		}
		const char *s = Z_STRVAL_P(value);
		const char *sep = strstr(s, "::");
		if (!sep) {
			zend_value_error("deepclone_from_array(): malformed payload, enum value must match \"Class::Case\"");
			return;
		}
		zend_string *class_name = zend_string_init_existing_interned(s, sep - s, 0);
		zend_string *case_name = zend_string_init_existing_interned(sep + 2, Z_STRLEN_P(value) - (sep - s) - 2, 0);
		zend_class_entry *ce = zend_lookup_class(class_name);
		if (!ce || !(ce->ce_flags & ZEND_ACC_ENUM)) {
			zend_string_release(class_name);
			zend_string_release(case_name);
			zend_value_error("deepclone_from_array(): malformed payload, enum class \"%s\" not found", s);
			return;
		}
		zend_object *case_obj = zend_enum_get_case(ce, case_name);
		zend_string_release(class_name);
		zend_string_release(case_name);
		if (!case_obj) {
			zend_value_error("deepclone_from_array(): malformed payload, enum case \"%s\" not found", s);
			return;
		}
		ZVAL_OBJ_COPY(retval, case_obj);
		return;
	}

	if (Z_TYPE_P(mask) != IS_ARRAY) {
		ZVAL_COPY(retval, value);
		return;
	}

	/* Array mask: recurse, handling & refs inline */
	if (Z_TYPE_P(value) != IS_ARRAY) {
		zend_value_error("deepclone_from_array(): malformed payload, array-mask value must be of type array, %s given", zend_zval_value_name(value));
		return;
	}
	zval result;
	ZVAL_DUP(&result, value);
	SEPARATE_ARRAY(&result);

	zend_string *mkey;
	zend_ulong midx;
	zval *mval;
	ZEND_HASH_FOREACH_KEY_VAL(Z_ARRVAL_P(mask), midx, mkey, mval) {
		zval *slot = mkey
			? zend_hash_find(Z_ARRVAL(result), mkey)
			: zend_hash_index_find(Z_ARRVAL(result), midx);
		if (!slot) continue;

		if (Z_TYPE_P(mval) == IS_FALSE) {
			/* Hard ref: create PHP & reference */
			if (Z_TYPE_P(slot) != IS_LONG) {
				zval_ptr_dtor(&result);
				zend_value_error("deepclone_from_array(): malformed payload, hard-ref slot must be of type int, %s given", zend_zval_value_name(slot));
				return;
			}
			if (UNEXPECTED(Z_LVAL_P(slot) <= ZEND_LONG_MIN || Z_LVAL_P(slot) >= 0)) {
				zval_ptr_dtor(&result);
				zend_value_error("deepclone_from_array(): malformed payload, ref id out of range");
				return;
			}
			zend_long rid = -Z_LVAL_P(slot);
			zval *ref_slot = zend_hash_index_find(refs, rid);
			if (!ref_slot) {
				zval_ptr_dtor(&result);
				zend_value_error("deepclone_from_array(): malformed payload, unknown ref id " ZEND_LONG_FMT, rid);
				return;
			}
			if (!Z_ISREF_P(ref_slot)) {
				ZVAL_MAKE_REF(ref_slot);
			}
			zval_ptr_dtor(slot);
			ZVAL_COPY(slot, ref_slot);
		} else {
			zval resolved;
			ZVAL_UNDEF(&resolved);
			dc_resolve(slot, mval, objects, num_objects, refs, &resolved);
			if (EG(exception)) {
				zval_ptr_dtor(&result);
				return;
			}
			if (!Z_ISUNDEF(resolved)) {
				zval_ptr_dtor(slot);
				ZVAL_COPY_VALUE(slot, &resolved);
			}
		}
	} ZEND_HASH_FOREACH_END();

	ZVAL_COPY_VALUE(retval, &result);
}

/* Throw a ValueError describing malformed input and jump to the cleanup label. */
#define DC_INVALID(...) do { \
		zend_value_error(__VA_ARGS__); \
		goto cleanup; \
	} while (0)
#define DC_REQUIRE(cond, ...) do { if (UNEXPECTED(!(cond))) DC_INVALID(__VA_ARGS__); } while (0)

/* Recursively scan a mask zval tree for LONG(0) entries (named-closure
 * markers). Returns true as soon as one is found. */
static bool dc_mask_has_closure(zval *mask)
{
	if (mask == NULL) {
		return false;
	}
	if (DC_MASK_IS_NAMED_CLOSURE(mask)) {
		return true;
	}
	if (Z_TYPE_P(mask) != IS_ARRAY) {
		return false;
	}
	zval *v;
	ZEND_HASH_FOREACH_VAL(Z_ARRVAL_P(mask), v) {
		if (dc_mask_has_closure(v)) {
			return true;
		}
	} ZEND_HASH_FOREACH_END();
	return false;
}

PHP_FUNCTION(deepclone_from_array)
{
	HashTable *data_ht;
	HashTable *allowed_ht = NULL;
	HashTable *allowed_set = NULL;
	HashTable refs;
	zend_string **class_names = NULL;
	uint32_t num_classes = 0;
	zend_class_entry **class_ces = NULL;
	zval *objects = NULL;
	uint32_t num_objects = 0;
	zend_string **obj_classes = NULL;
	int *obj_wakeups = NULL;
	bool refs_inited = false;

	ZEND_PARSE_PARAMETERS_START(1, 2)
		Z_PARAM_ARRAY_HT(data_ht)
		Z_PARAM_OPTIONAL
		Z_PARAM_ARRAY_HT_OR_NULL(allowed_ht)
	ZEND_PARSE_PARAMETERS_END();

	/* Static value: return data['value'] */
	zval *zvalue = zend_hash_find_known_hash(data_ht, dc_key_value);
	if (zvalue) {
		ZVAL_COPY(return_value, zvalue);
		return;
	}

	/* ── Parse and validate the format ─────────── */
	zval *zclasses     = zend_hash_find_known_hash(data_ht, dc_key_classes);
	zval *zobject_meta = zend_hash_find_known_hash(data_ht, dc_key_object_meta);
	zval *zprepared    = zend_hash_find_known_hash(data_ht, dc_key_prepared);
	zval *zmask        = zend_hash_find_known_hash(data_ht, dc_key_mask);
	zval *zproperties  = zend_hash_find_known_hash(data_ht, dc_key_properties);
	zval *zresolve     = zend_hash_find_known_hash(data_ht, dc_key_resolve);
	zval *zstates      = zend_hash_find_known_hash(data_ht, dc_key_states);
	zval *zrefs        = zend_hash_find_known_hash(data_ht, dc_key_refs);
	zval *zref_masks   = zend_hash_find_known_hash(data_ht, dc_key_ref_masks);

	DC_REQUIRE(zclasses,     "deepclone_from_array(): Argument #1 ($data) is missing required \"classes\" key");
	DC_REQUIRE(zobject_meta, "deepclone_from_array(): Argument #1 ($data) is missing required \"objectMeta\" key");
	DC_REQUIRE(zprepared,    "deepclone_from_array(): Argument #1 ($data) is missing required \"prepared\" key");
	DC_REQUIRE(Z_TYPE_P(zclasses) == IS_STRING || Z_TYPE_P(zclasses) == IS_ARRAY,
		"deepclone_from_array(): Argument #1 ($data) \"classes\" must be of type string|array, %s given", zend_zval_value_name(zclasses));
	DC_REQUIRE(Z_TYPE_P(zobject_meta) == IS_LONG || Z_TYPE_P(zobject_meta) == IS_ARRAY,
		"deepclone_from_array(): Argument #1 ($data) \"objectMeta\" must be of type int|array, %s given", zend_zval_value_name(zobject_meta));
	DC_REQUIRE(!zproperties || Z_TYPE_P(zproperties) == IS_ARRAY,
		"deepclone_from_array(): Argument #1 ($data) \"properties\" must be of type array, %s given", zend_zval_value_name(zproperties));
	DC_REQUIRE(!zresolve || Z_TYPE_P(zresolve) == IS_ARRAY,
		"deepclone_from_array(): Argument #1 ($data) \"resolve\" must be of type array, %s given", zend_zval_value_name(zresolve));
	DC_REQUIRE(!zstates || Z_TYPE_P(zstates) == IS_ARRAY,
		"deepclone_from_array(): Argument #1 ($data) \"states\" must be of type array, %s given", zend_zval_value_name(zstates));
	DC_REQUIRE(!zrefs || Z_TYPE_P(zrefs) == IS_ARRAY,
		"deepclone_from_array(): Argument #1 ($data) \"refs\" must be of type array, %s given", zend_zval_value_name(zrefs));
	DC_REQUIRE(!zref_masks || Z_TYPE_P(zref_masks) == IS_ARRAY,
		"deepclone_from_array(): Argument #1 ($data) \"refMasks\" must be of type array, %s given", zend_zval_value_name(zref_masks));

	/* ── Expand class names into a flat C array ── */
	if (Z_TYPE_P(zclasses) == IS_STRING) {
		if (Z_STRLEN_P(zclasses) > 0) {
			num_classes = 1;
			class_names = emalloc(sizeof(zend_string *));
			class_names[0] = Z_STR_P(zclasses);
		}
	} else {
		num_classes = zend_hash_num_elements(Z_ARRVAL_P(zclasses));
		if (num_classes) {
			class_names = emalloc(num_classes * sizeof(zend_string *));
			uint32_t i = 0;
			zval *cls;
			ZEND_HASH_FOREACH_VAL(Z_ARRVAL_P(zclasses), cls) {
				if (Z_TYPE_P(cls) != IS_STRING) {
					DC_INVALID("deepclone_from_array(): Argument #1 ($data) \"classes\" entries must be of type string, %s given", zend_zval_value_name(cls));
				}
				class_names[i++] = Z_STR_P(cls);
			} ZEND_HASH_FOREACH_END();
		}
	}

	/* ── Validate allowed classes ──────────────── */
	if (allowed_ht) {
		allowed_set = dc_build_allowed_set(allowed_ht, "deepclone_from_array");
		if (!allowed_set) {
			goto cleanup;
		}

		for (uint32_t i = 0; i < num_classes; i++) {
			if (!dc_class_allowed(allowed_set, class_names[i])) {
				DC_INVALID("deepclone_from_array(): class \"%s\" is not allowed", ZSTR_VAL(class_names[i]));
			}
		}

		bool closure_allowed = dc_class_allowed(allowed_set, zend_ce_closure->name);

		if (!closure_allowed) {
			/* Scan mask, resolve, refMasks, and state masks for the
			 * named-closure marker (IS_LONG, value 0). */
			if (dc_mask_has_closure(zmask)) {
				DC_INVALID("deepclone_from_array(): class \"Closure\" is not allowed");
			}
			if (zresolve) {
				zval *scope;
				ZEND_HASH_FOREACH_VAL(Z_ARRVAL_P(zresolve), scope) {
					if (Z_TYPE_P(scope) != IS_ARRAY) continue;
					zval *name;
					ZEND_HASH_FOREACH_VAL(Z_ARRVAL_P(scope), name) {
						if (dc_mask_has_closure(name)) {
							DC_INVALID("deepclone_from_array(): class \"Closure\" is not allowed");
						}
					} ZEND_HASH_FOREACH_END();
				} ZEND_HASH_FOREACH_END();
			}
			if (zref_masks) {
				zval *rmask;
				ZEND_HASH_FOREACH_VAL(Z_ARRVAL_P(zref_masks), rmask) {
					if (dc_mask_has_closure(rmask)) {
						DC_INVALID("deepclone_from_array(): class \"Closure\" is not allowed");
					}
				} ZEND_HASH_FOREACH_END();
			}
			if (zstates) {
				zval *state;
				ZEND_HASH_FOREACH_VAL(Z_ARRVAL_P(zstates), state) {
					if (Z_TYPE_P(state) == IS_ARRAY) {
						zval *smask = zend_hash_index_find(Z_ARRVAL_P(state), 2);
						if (smask && dc_mask_has_closure(smask)) {
							DC_INVALID("deepclone_from_array(): class \"Closure\" is not allowed");
						}
					}
				} ZEND_HASH_FOREACH_END();
			}
		}
	}

	/* ── Build objectMeta ── */
	if (Z_TYPE_P(zobject_meta) == IS_LONG) {
		zend_long n = Z_LVAL_P(zobject_meta);
		if (n < 0) {
			DC_INVALID("deepclone_from_array(): Argument #1 ($data) \"objectMeta\" count must be non-negative, " ZEND_LONG_FMT " given", n);
		}
		/* On 64-bit zend_long is int64_t and n can exceed UINT32_MAX; on
		 * 32-bit it's int32_t and the comparison is tautologically false
		 * (-Werror=type-limits would reject it), so guard the upper bound. */
#if SIZEOF_ZEND_LONG > 4
		if (n > (zend_long)UINT32_MAX) {
			DC_INVALID("deepclone_from_array(): Argument #1 ($data) \"objectMeta\" count out of range: " ZEND_LONG_FMT, n);
		}
#endif
		num_objects = (uint32_t) n;
		if (num_objects > 0) {
			if (num_classes < 1) {
				DC_INVALID("deepclone_from_array(): Argument #1 ($data) \"objectMeta\" references class index 0 but \"classes\" is empty");
			}
			obj_classes = emalloc(num_objects * sizeof(zend_string *));
			obj_wakeups = ecalloc(num_objects, sizeof(int));
			for (uint32_t i = 0; i < num_objects; i++) {
				obj_classes[i] = class_names[0];
			}
		}
	} else {
		num_objects = zend_hash_num_elements(Z_ARRVAL_P(zobject_meta));
		if (num_objects > 0) {
			obj_classes = emalloc(num_objects * sizeof(zend_string *));
			obj_wakeups = ecalloc(num_objects, sizeof(int));
		}
		zend_ulong id;
		zval *meta;
		ZEND_HASH_FOREACH_NUM_KEY_VAL(Z_ARRVAL_P(zobject_meta), id, meta) {
			if (id >= num_objects) {
				DC_INVALID("deepclone_from_array(): Argument #1 ($data) \"objectMeta\" entry index " ZEND_ULONG_FMT " out of range", id);
			}
			zend_long cidx_val;
			if (Z_TYPE_P(meta) == IS_ARRAY) {
				zval *cidx = zend_hash_index_find(Z_ARRVAL_P(meta), 0);
				zval *wk   = zend_hash_index_find(Z_ARRVAL_P(meta), 1);
				if (!cidx || Z_TYPE_P(cidx) != IS_LONG || !wk || Z_TYPE_P(wk) != IS_LONG) {
					DC_INVALID("deepclone_from_array(): Argument #1 ($data) \"objectMeta\" entry " ZEND_ULONG_FMT " must be [int, int]", id);
				}
				cidx_val = Z_LVAL_P(cidx);
				obj_wakeups[id] = (int) Z_LVAL_P(wk);
			} else if (Z_TYPE_P(meta) == IS_LONG) {
				cidx_val = Z_LVAL_P(meta);
			} else {
				DC_INVALID("deepclone_from_array(): Argument #1 ($data) \"objectMeta\" entry " ZEND_ULONG_FMT " must be of type int|array, %s given", id, zend_zval_value_name(meta));
			}
			if (cidx_val < 0 || (zend_ulong) cidx_val >= num_classes) {
				DC_INVALID("deepclone_from_array(): Argument #1 ($data) \"objectMeta\" entry " ZEND_ULONG_FMT " has out-of-range class index " ZEND_LONG_FMT, id, cidx_val);
			}
			obj_classes[id] = class_names[cidx_val];
		} ZEND_HASH_FOREACH_END();
	}

	/* ── Resolve class entries (once per unique class) ── */
	if (num_classes) {
		class_ces = ecalloc(num_classes, sizeof(zend_class_entry *));
	}

	/* ── Initialize refs early so cleanup can always destroy it safely ── */
	zend_hash_init(&refs, 4, NULL, ZVAL_PTR_DTOR, 0);
	refs_inited = true;

	/* ── Create object instances ───────────────── */
	if (num_objects) {
		objects = emalloc(num_objects * sizeof(zval));
		for (uint32_t i = 0; i < num_objects; i++) {
			ZVAL_UNDEF(&objects[i]);
		}
	}

	for (uint32_t id = 0; id < num_objects; id++) {
		zend_string *class_name = obj_classes[id];
		zval obj_zval;

		if (ZSTR_LEN(class_name) > 1 && ZSTR_VAL(class_name)[1] == ':') {
			php_unserialize_data_t var_hash;
			PHP_VAR_UNSERIALIZE_INIT(var_hash);
			if (allowed_set) {
				php_var_unserialize_set_allowed_classes(var_hash, allowed_set);
			}
			const unsigned char *p = (const unsigned char *)ZSTR_VAL(class_name);
			const unsigned char *end = p + ZSTR_LEN(class_name);
			if (!php_var_unserialize(&obj_zval, &p, end, &var_hash)) {
				PHP_VAR_UNSERIALIZE_DESTROY(var_hash);
				DC_INVALID("deepclone_from_array(): Argument #1 ($data) failed to unserialize object %u", id);
			}
			PHP_VAR_UNSERIALIZE_DESTROY(var_hash);
		} else {
			/* Look up CE, caching by class_id (parallel to class_names). */
			zend_class_entry *ce = NULL;
			for (uint32_t ci = 0; ci < num_classes; ci++) {
				if (class_names[ci] == class_name) {
					ce = class_ces[ci];
					if (!ce) {
						ce = zend_lookup_class(class_name);
						if (!ce) {
							zend_throw_exception_ex(dc_ce_class_not_found_exception, 0,
								"Class \"%s\" not found.", ZSTR_VAL(class_name));
							goto cleanup;
						}
						class_ces[ci] = ce;
					}
					break;
				}
			}
			if (UNEXPECTED(object_init_ex(&obj_zval, ce) != SUCCESS)) {
				goto cleanup;
			}
		}

		ZVAL_COPY_VALUE(&objects[id], &obj_zval);
	}

	/* ── Resolve refs ──────────────────────────── */
	if (zrefs && Z_TYPE_P(zrefs) == IS_ARRAY) {
		/* First pass: populate refs with unresolved copies (needed for self-refs) */
		zend_ulong rid;
		zval *rval;
		ZEND_HASH_FOREACH_NUM_KEY_VAL(Z_ARRVAL_P(zrefs), rid, rval) {
			zval copy;
			ZVAL_COPY(&copy, rval);
			zend_hash_index_add_new(&refs, rid, &copy);
		} ZEND_HASH_FOREACH_END();

		/* Second pass: resolve those with masks, updating in-place */
		if (zref_masks) {
			zval *rmask;
			ZEND_HASH_FOREACH_NUM_KEY_VAL(Z_ARRVAL_P(zref_masks), rid, rmask) {
				zval *slot = zend_hash_index_find(&refs, rid);
				if (!slot) continue;
				zval resolved;
				ZVAL_UNDEF(&resolved);
				dc_resolve(slot, rmask, objects, num_objects, &refs, &resolved);
				if (EG(exception)) goto cleanup;
				/* Write through reference if slot was made into one (by dc_resolve) */
				if (Z_ISREF_P(slot)) {
					zval *inner = Z_REFVAL_P(slot);
					zval_ptr_dtor(inner);
					ZVAL_COPY_VALUE(inner, &resolved);
				} else {
					zval_ptr_dtor(slot);
					ZVAL_COPY_VALUE(slot, &resolved);
				}
			} ZEND_HASH_FOREACH_END();
		}
	}

	/* ── Hydrate properties ────────────────────── */
	if (zproperties) {
		zend_string *scope_name;
		zval *scope_props;
		ZEND_HASH_FOREACH_STR_KEY_VAL(Z_ARRVAL_P(zproperties), scope_name, scope_props) {
			if (!scope_name) {
				DC_INVALID("deepclone_from_array(): Argument #1 ($data) \"properties\" keys must be of type string");
			}
			if (Z_TYPE_P(scope_props) != IS_ARRAY) {
				DC_INVALID("deepclone_from_array(): Argument #1 ($data) \"properties\" entry for scope \"%s\" must be of type array, %s given", ZSTR_VAL(scope_name), zend_zval_value_name(scope_props));
			}

			/* Resolve object refs in this scope's properties */
			HashTable *resolve_scope = NULL;
			if (zresolve) {
				zval *rs = zend_hash_find(Z_ARRVAL_P(zresolve), scope_name);
				if (rs) {
					if (Z_TYPE_P(rs) != IS_ARRAY) {
						DC_INVALID("deepclone_from_array(): Argument #1 ($data) \"resolve\" entry for scope \"%s\" must be of type array, %s given", ZSTR_VAL(scope_name), zend_zval_value_name(rs));
					}
					resolve_scope = Z_ARRVAL_P(rs);
				}
			}

			zend_class_entry *scope_ce = NULL;
			for (uint32_t ci = 0; ci < num_classes; ci++) {
				if (zend_string_equals(class_names[ci], scope_name)) {
					scope_ce = class_ces[ci];
					break;
				}
			}
			if (!scope_ce) {
				/* Look up without triggering autoload — scopes reference
				 * classes that must already be loaded (they're parents of
				 * validated objects, or stdClass). Uses the CE cache on
				 * scope_name for O(1) repeat lookups. */
				scope_ce = zend_lookup_class_ex(scope_name, NULL, ZEND_FETCH_CLASS_NO_AUTOLOAD);
			}
			DC_REQUIRE(scope_ce,
				"deepclone_from_array(): Argument #1 ($data) \"properties\" scope \"%s\" is not a loaded class name",
				ZSTR_VAL(scope_name));
			bool scope_is_std = scope_ce == zend_standard_class_def;
			/* PHP 8.5+ made EG(fake_scope) a const pointer (#19060). The
			 * shim casts the read so we keep one source for both worlds. */
#if PHP_VERSION_ID >= 80500
			const zend_class_entry *old_scope = EG(fake_scope);
#else
			zend_class_entry *old_scope = EG(fake_scope);
#endif
			if (scope_ce && scope_ce != zend_standard_class_def) {
				EG(fake_scope) = scope_ce;
			}

			zend_string *prop_name;
			zval *id_values;
			ZEND_HASH_FOREACH_STR_KEY_VAL(Z_ARRVAL_P(scope_props), prop_name, id_values) {
				if (!prop_name) {
					EG(fake_scope) = old_scope;
					DC_INVALID("deepclone_from_array(): Argument #1 ($data) \"properties\" inner keys must be of type string");
				}
				if (Z_TYPE_P(id_values) != IS_ARRAY) {
					EG(fake_scope) = old_scope;
					DC_INVALID("deepclone_from_array(): Argument #1 ($data) \"properties\" value for \"%s::%s\" must be of type array, %s given", ZSTR_VAL(scope_name), ZSTR_VAL(prop_name), zend_zval_value_name(id_values));
				}

				/* Get resolve markers for this property */
				HashTable *resolve_ids = NULL;
				if (resolve_scope) {
					zval *ri = zend_hash_find(resolve_scope, prop_name);
					if (ri) {
						if (Z_TYPE_P(ri) != IS_ARRAY) {
							EG(fake_scope) = old_scope;
							DC_INVALID("deepclone_from_array(): Argument #1 ($data) \"resolve\" value for \"%s::%s\" must be of type array, %s given", ZSTR_VAL(scope_name), ZSTR_VAL(prop_name), zend_zval_value_name(ri));
						}
						resolve_ids = Z_ARRVAL_P(ri);
					}
				}

				/* Hoist property_info lookup — same for all object ids in this
				 * scope+prop combination. Declared property names are interned
				 * in &scope_ce->properties_info so _known_hash is safe. */
				zend_property_info *pi = NULL;
				if (scope_ce && scope_ce != zend_standard_class_def) {
					zval *zv = zend_hash_find_known_hash(&scope_ce->properties_info, prop_name);
					if (zv) {
						zend_property_info *candidate = Z_PTR_P(zv);
						if (!(candidate->flags & ZEND_ACC_STATIC)) {
							pi = candidate;
						}
					}
				}

				zend_ulong obj_id;
				zval *prop_val;
				ZEND_HASH_FOREACH_NUM_KEY_VAL(Z_ARRVAL_P(id_values), obj_id, prop_val) {
					if (obj_id >= num_objects) continue;
					zval *obj_zval = &objects[obj_id];
					zend_object *obj = Z_OBJ_P(obj_zval);

					if (!scope_is_std && !instanceof_function(obj->ce, scope_ce)) {
						EG(fake_scope) = old_scope;
						DC_INVALID("deepclone_from_array(): Argument #1 ($data) \"properties\" scope \"%s\" is not a parent of object id " ZEND_ULONG_FMT " (%s)",
							ZSTR_VAL(scope_name), obj_id, ZSTR_VAL(obj->ce->name));
					}

					zval final_val;
					zval *marker = resolve_ids ? zend_hash_index_find(resolve_ids, obj_id) : NULL;
					if (marker) {
						ZVAL_UNDEF(&final_val);
						dc_resolve(prop_val, marker, objects, num_objects, &refs, &final_val);
						if (EG(exception)) {
							EG(fake_scope) = old_scope;
							goto cleanup;
						}
					} else {
						ZVAL_COPY(&final_val, prop_val);
					}

					/* Write property to object */
					/* For stdClass-scoped public properties on typed classes,
					 * look up pi via obj->ce — this also preserves references
					 * (zend_std_write_property can't accept IS_REFERENCE). */
					zend_property_info *use_pi = pi;
					if (!use_pi && scope_is_std && obj->ce != zend_standard_class_def) {
						zval *zv = zend_hash_find_known_hash(&obj->ce->properties_info, prop_name);
						if (zv) {
							zend_property_info *candidate = Z_PTR_P(zv);
							if (dc_is_std_scope_property(candidate)) {
								use_pi = candidate;
							} else {
								zval_ptr_dtor(&final_val);
								EG(fake_scope) = old_scope;
								DC_INVALID("deepclone_from_array(): Argument #1 ($data) \"properties\" value for \"%s::%s\" targets a non-public declared property on object id " ZEND_ULONG_FMT,
									ZSTR_VAL(scope_name), ZSTR_VAL(prop_name), obj_id);
							}
						}
					} else if (!use_pi && !scope_is_std) {
						zval_ptr_dtor(&final_val);
						EG(fake_scope) = old_scope;
						DC_INVALID("deepclone_from_array(): Argument #1 ($data) \"properties\" value for \"%s::%s\" does not match a declared property on object id " ZEND_ULONG_FMT,
							ZSTR_VAL(scope_name), ZSTR_VAL(prop_name), obj_id);
					}
					if (dc_is_backed_declared_property(use_pi)) {
						/* deepclone_from_array always uses setRawValue semantics
						 * (flags=0): payload-driven, same policy as unserialize(). */
						bool ok = dc_write_backed_property(obj, use_pi, prop_name, &final_val, 0);
						zval_ptr_dtor(&final_val);
						if (UNEXPECTED(!ok)) {
							EG(fake_scope) = old_scope;
							goto cleanup;
						}
					} else if (scope_is_std && obj->ce == zend_standard_class_def) {
						if (UNEXPECTED(!obj->properties)) {
							rebuild_object_properties_internal(obj);
						}
						zend_hash_update(obj->properties, prop_name, &final_val);
					} else {
						zend_std_write_property(obj, prop_name, &final_val, NULL);
						zval_ptr_dtor(&final_val);
						if (EG(exception)) {
							EG(fake_scope) = old_scope;
							goto cleanup;
						}
					}
				} ZEND_HASH_FOREACH_END();
			} ZEND_HASH_FOREACH_END();

			EG(fake_scope) = old_scope;
		} ZEND_HASH_FOREACH_END();
	}

	/* ── States: __unserialize / __wakeup ──────── */
	if (zstates) {
		zval *state;
		ZEND_HASH_FOREACH_VAL(Z_ARRVAL_P(zstates), state) {
			if (Z_TYPE_P(state) == IS_ARRAY) {
				/* __unserialize: [id, props, mask?] */
				zval *zid    = zend_hash_index_find(Z_ARRVAL_P(state), 0);
				zval *sprops = zend_hash_index_find(Z_ARRVAL_P(state), 1);
				zval *smask  = zend_hash_index_find(Z_ARRVAL_P(state), 2);
				if (!zid || Z_TYPE_P(zid) != IS_LONG || !sprops) {
					DC_INVALID("deepclone_from_array(): Argument #1 ($data) malformed \"states\" entry: expected [int, mixed, mixed?]");
				}
				if (Z_LVAL_P(zid) < 0 || (zend_ulong) Z_LVAL_P(zid) >= num_objects) {
					DC_INVALID("deepclone_from_array(): Argument #1 ($data) \"states\" entry references unknown object id " ZEND_LONG_FMT, Z_LVAL_P(zid));
				}
				zval *obj_zval = &objects[Z_LVAL_P(zid)];
				zend_class_entry *unser_ce = Z_OBJCE_P(obj_zval);
				if (!unser_ce->__unserialize) {
					DC_INVALID("deepclone_from_array(): Argument #1 ($data) \"states\" entry references object id " ZEND_LONG_FMT " whose class %s has no __unserialize() method", Z_LVAL_P(zid), ZSTR_VAL(unser_ce->name));
				}
				zval resolved_props;
				if (smask) {
					ZVAL_UNDEF(&resolved_props);
					dc_resolve(sprops, smask, objects, num_objects, &refs, &resolved_props);
					if (EG(exception)) goto cleanup;
				} else {
					ZVAL_COPY(&resolved_props, sprops);
				}
				zend_call_method_with_1_params(Z_OBJ_P(obj_zval), unser_ce,
					&unser_ce->__unserialize, "__unserialize", NULL, &resolved_props);
				zval_ptr_dtor(&resolved_props);
				if (EG(exception)) goto cleanup;
			} else if (Z_TYPE_P(state) == IS_LONG) {
				/* __wakeup: just the object ID */
				if (Z_LVAL_P(state) < 0 || (zend_ulong) Z_LVAL_P(state) >= num_objects) {
					DC_INVALID("deepclone_from_array(): Argument #1 ($data) \"states\" entry references unknown object id " ZEND_LONG_FMT, Z_LVAL_P(state));
				}
				zval *obj_zval = &objects[Z_LVAL_P(state)];
				zend_class_entry *wakeup_ce = Z_OBJCE_P(obj_zval);
				zend_function *wakeup_fn = zend_hash_find_ptr(&wakeup_ce->function_table, ZSTR_KNOWN(ZEND_STR_WAKEUP));
				if (wakeup_fn) {
					zend_call_method_with_0_params(Z_OBJ_P(obj_zval), wakeup_ce,
						&wakeup_fn, "__wakeup", NULL);
					if (EG(exception)) goto cleanup;
				}
			} else {
				DC_INVALID("deepclone_from_array(): Argument #1 ($data) \"states\" entry must be of type int|array, %s given", zend_zval_value_name(state));
			}
		} ZEND_HASH_FOREACH_END();
	}

	/* ── Resolve prepared tree ─────────────────── */
	if (Z_TYPE_P(zprepared) == IS_LONG) {
		zend_long id = Z_LVAL_P(zprepared);
		if (id >= 0) {
			if ((zend_ulong) id >= num_objects) {
				DC_INVALID("deepclone_from_array(): Argument #1 ($data) \"prepared\" references unknown object id " ZEND_LONG_FMT, id);
			}
			zval *obj = &objects[id];
			ZVAL_COPY(return_value, obj);
		} else {
			zval *ref = zend_hash_index_find(&refs, -id);
			if (!ref) {
				DC_INVALID("deepclone_from_array(): Argument #1 ($data) \"prepared\" references unknown ref id " ZEND_LONG_FMT, -id);
			}
			ZVAL_COPY(return_value, ref);
		}
	} else if (zmask) {
		dc_resolve(zprepared, zmask, objects, num_objects, &refs, return_value);
		if (EG(exception)) goto cleanup;
	} else {
		ZVAL_COPY(return_value, zprepared);
	}

cleanup:
	if (allowed_set) { zend_hash_destroy(allowed_set); efree(allowed_set); }
	if (refs_inited) zend_hash_destroy(&refs);
	if (objects) {
		for (uint32_t i = 0; i < num_objects; i++) {
			zval_ptr_dtor(&objects[i]);
		}
		efree(objects);
	}
	if (obj_classes) efree(obj_classes);
	if (obj_wakeups) efree(obj_wakeups);
	if (class_ces)   efree(class_ces);
	if (class_names) efree(class_names);
}
#undef DC_INVALID

/* ── deepclone_hydrate() — instantiate/hydrate with scoped property writes ── */

PHP_FUNCTION(deepclone_hydrate)
{
	zend_object *obj_arg = NULL;
	zend_string *class_name = NULL;
	HashTable *vars = NULL;
	zend_long flags = 0;

	ZEND_PARSE_PARAMETERS_START(1, 3)
		Z_PARAM_OBJ_OR_STR(obj_arg, class_name)
		Z_PARAM_OPTIONAL
		Z_PARAM_ARRAY_HT(vars)
		Z_PARAM_LONG(flags)
	ZEND_PARSE_PARAMETERS_END();

	if (UNEXPECTED(flags & ~(zend_long) DEEPCLONE_HYDRATE_FLAGS_MASK)) {
		zend_value_error("deepclone_hydrate(): Argument #3 ($flags) contains unknown bits");
		RETURN_THROWS();
	}
	if (UNEXPECTED((flags & DEEPCLONE_HYDRATE_CALL_HOOKS) && (flags & DEEPCLONE_HYDRATE_NO_LAZY_INIT))) {
		zend_value_error("deepclone_hydrate(): Argument #3 ($flags) DEEPCLONE_HYDRATE_CALL_HOOKS and DEEPCLONE_HYDRATE_NO_LAZY_INIT are mutually exclusive");
		RETURN_THROWS();
	}

	/* In MANGLED_VARS mode $vars is a flat mangled-key array (the shape
	 * (array) $obj produces). Otherwise it's the scoped per-class shape. */
	bool mangled_vars_mode = (flags & DEEPCLONE_HYDRATE_MANGLED_VARS) != 0;
	HashTable *scoped_props = mangled_vars_mode ? NULL : vars;
	HashTable *mangled_vars = mangled_vars_mode ? vars : NULL;

	zval obj_zval;

	if (EXPECTED(obj_arg)) {
		ZVAL_OBJ_COPY(&obj_zval, obj_arg);
	} else {
		zend_class_entry *ce = zend_lookup_class(class_name);
		if (UNEXPECTED(!ce)) {
			zend_throw_exception_ex(dc_ce_class_not_found_exception, 0,
				"Class \"%s\" not found.", ZSTR_VAL(class_name));
			RETURN_THROWS();
		}
		if (UNEXPECTED(ce->ce_flags & ZEND_ACC_UNINSTANTIABLE)) {
			zend_throw_exception_ex(dc_ce_not_instantiable_exception, 0,
				"Class \"%s\" is not instantiable.", ZSTR_VAL(ce->name));
			RETURN_THROWS();
		}
		/* Reject classes that cannot function without their constructor,
		 * using the same rules as dc_get_class_info / deepclone_from_array.
		 * User classes always pass; internal classes are checked and cached. */
		if (UNEXPECTED(ce->type == ZEND_INTERNAL_CLASS)) {
			/* Per-thread cache (via module globals). Packs ce pointer + ok-bit
			 * into the stored value: low bit is ok, high bits are the ce. A ce
			 * mismatch means the entry is stale (e.g. shared extension reloaded
			 * the class between requests) — recompute. */
			HashTable *hydrate_cache = &DC_G(hydrate_cache);
			uintptr_t packed = (uintptr_t) zend_hash_find_ptr(hydrate_cache, ce->name);
			zend_class_entry *cached_ce = (zend_class_entry *) (packed & ~(uintptr_t) 1);
			if (EXPECTED(cached_ce == ce)) {
				if (UNEXPECTED(!(packed & 1))) {
					zend_throw_exception_ex(dc_ce_not_instantiable_exception, 0,
						"Class \"%s\" is not instantiable.", ZSTR_VAL(ce->name));
					RETURN_THROWS();
				}
			} else {
				bool ok = true;
				bool has_unser = ce->__unserialize != NULL;
				bool has_wakeup = zend_hash_find_known_hash(&ce->function_table, ZSTR_KNOWN(ZEND_STR_WAKEUP)) != NULL;
				bool has_ser_api = has_unser || has_wakeup || ce->serialize != NULL;
				if (!has_ser_api
				 && ((ce->ce_flags & ZEND_ACC_ANON_CLASS)
				  || instanceof_function(ce, reflector_ptr)
				  || instanceof_function(ce, reflection_type_ptr)
				  || instanceof_function(ce, spl_ce_IteratorIterator)
				  || instanceof_function(ce, spl_ce_RecursiveIteratorIterator)
				  || (dc_ce_reflection_generator && instanceof_function(ce, dc_ce_reflection_generator)))) {
					ok = false;
				}
				if (ok && (ce->ce_flags & ZEND_ACC_NOT_SERIALIZABLE) && !has_ser_api) {
					ok = false;
				}
				if (ok && ce->create_object != NULL && ce != php_ce_incomplete_class && !has_ser_api
				 && !(ce->__serialize) && !(zend_hash_find_known_hash(&ce->function_table, ZSTR_KNOWN(ZEND_STR_SLEEP)))) {
					if (ce->ce_flags & ZEND_ACC_FINAL) {
						zval probe;
						if (object_init_ex(&probe, ce) != SUCCESS || EG(exception)) {
							zend_clear_exception();
							ok = false;
						} else {
							zval_ptr_dtor(&probe);
						}
					} else {
						ok = false;
					}
				}
				packed = (uintptr_t) ce | (ok ? 1 : 0);
				zend_hash_update_ptr(hydrate_cache, ce->name, (void *) packed);
				if (!ok) {
					zend_throw_exception_ex(dc_ce_not_instantiable_exception, 0,
						"Class \"%s\" is not instantiable.", ZSTR_VAL(ce->name));
					RETURN_THROWS();
				}
			}
		}
		if (UNEXPECTED(object_init_ex(&obj_zval, ce) != SUCCESS)) {
			RETURN_THROWS();
		}

	}

	/* Resolve $mangled_vars (flat mangled-key array) into scoped_props.
	 *   "\0ClassName\0propName" → scope = ClassName
	 *   "\0*\0propName"         → scope = declaring class (or object class)
	 *   "propName"              → scope = declaring class (or object class)
	 */
	zval local_scoped;
	bool scoped_owned = false;
	if (mangled_vars && zend_hash_num_elements(mangled_vars)) {
		zend_class_entry *obj_ce = Z_OBJCE(obj_zval);

		if (!scoped_props || !zend_hash_num_elements(scoped_props)) {
			array_init(&local_scoped);
			scoped_props = Z_ARRVAL(local_scoped);
			scoped_owned = true;
		} else {
			/* Copy scoped_props so we can add to it without modifying the caller's array */
			array_init(&local_scoped);
			zend_hash_copy(Z_ARRVAL(local_scoped), scoped_props, zval_add_ref);
			scoped_props = Z_ARRVAL(local_scoped);
			scoped_owned = true;
		}

		zend_string *prop_key;
		zval *prop_val;
		ZEND_HASH_FOREACH_STR_KEY_VAL(mangled_vars, prop_key, prop_val) {
			if (UNEXPECTED(!prop_key)) {
				zend_value_error("deepclone_hydrate(): Argument #2 ($vars) in MANGLED_VARS mode must have only string keys");
				if (scoped_owned) zval_ptr_dtor(&local_scoped);
				zval_ptr_dtor(&obj_zval);
				RETURN_THROWS();
			}

			const char *key = ZSTR_VAL(prop_key);
			size_t key_len = ZSTR_LEN(prop_key);
			zend_string *scope_str;
			zend_string *real_name;

			if (key_len > 0 && key[0] == '\0') {
				if (key_len == 1) {
					/* Special "\0" key (SplObjectStorage/ArrayObject/ArrayIterator) —
					 * route to object's own class scope with key "\0" */
					scope_str = obj_ce->name;
					real_name = prop_key; /* keep as-is */
					goto add_to_scope;
				}

				/* Find second \0 separator */
				const char *sep = memchr(key + 1, '\0', key_len - 1);
				if (!sep || sep == key + 1) {
					zend_value_error("deepclone_hydrate(): Argument #2 ($vars) in MANGLED_VARS mode contains an invalid mangled key");
					if (scoped_owned) zval_ptr_dtor(&local_scoped);
					zval_ptr_dtor(&obj_zval);
					RETURN_THROWS();
				}
				size_t class_len = sep - (key + 1);
				size_t name_len = key_len - class_len - 2;

				/* Reject embedded NUL in the property name portion */
				if (UNEXPECTED(memchr(sep + 1, '\0', name_len) != NULL)) {
					zend_value_error("deepclone_hydrate(): Argument #2 ($vars) in MANGLED_VARS mode contains an invalid mangled key");
					if (scoped_owned) zval_ptr_dtor(&local_scoped);
					zval_ptr_dtor(&obj_zval);
					RETURN_THROWS();
				}
				real_name = zend_string_init(sep + 1, name_len, 0);

				if (class_len == 1 && key[1] == '*') {
					/* Protected: "\0*\0propName" — find declaring class */
					zend_property_info *pi = zend_hash_find_ptr(&obj_ce->properties_info, real_name);
					if (pi && !(pi->flags & ZEND_ACC_STATIC)) {
						scope_str = pi->ce->name;
					} else {
						scope_str = obj_ce->name;
					}
				} else {
					/* Private: "\0ClassName\0propName" */
					scope_str = zend_string_init(key + 1, class_len, 0);
				}
			} else {
				/* Bare name — find declaring class */
				real_name = prop_key;
				zend_property_info *pi = zend_hash_find_ptr(&obj_ce->properties_info, real_name);

				/* Fast path: the resolved property has a real backing slot (typed or
				 * non-hooked). Write directly via dc_write_backed_property and skip the
				 * scoped_props accumulation + second-pass write. This is the hot case
				 * for Doctrine-style `fetchAll()` flows where the row is a flat dict
				 * of declared field names. */
				if (pi && !(pi->flags & ZEND_ACC_STATIC) && dc_is_backed_declared_property(pi)) {
					zval *v = prop_val;
					if (!(flags & DEEPCLONE_HYDRATE_PRESERVE_REFS)) {
						ZVAL_DEREF(v);
					}
					if (UNEXPECTED(!dc_write_backed_property(Z_OBJ(obj_zval), pi, real_name, v, flags))) {
						if (scoped_owned) zval_ptr_dtor(&local_scoped);
						zval_ptr_dtor(&obj_zval);
						RETURN_THROWS();
					}
					continue;
				}

				if (pi && !(pi->flags & ZEND_ACC_STATIC)) {
					scope_str = pi->ce->name;
					/* For private-set properties, use the declaring class as write scope */
					if (pi->flags & ZEND_ACC_PRIVATE_SET) {
						scope_str = pi->ce->name;
					}
				} else {
					scope_str = obj_ce->name;
				}
				real_name = NULL; /* don't free — it's prop_key */
			}

add_to_scope:
			/* Add to scoped_props[scope_str][real_name] = &prop_val */
			zval *scope_bucket = zend_hash_find(scoped_props, scope_str);
			if (!scope_bucket) {
				zval new_arr;
				array_init(&new_arr);
				scope_bucket = zend_hash_update(scoped_props, scope_str, &new_arr);
			}
			if (UNEXPECTED(Z_TYPE_P(scope_bucket) != IS_ARRAY)) {
				zend_value_error("deepclone_hydrate(): Argument #2 ($vars) value for scope \"%s\" must be of type array, %s given",
					ZSTR_VAL(scope_str), zend_zval_value_name(scope_bucket));
				if (real_name && real_name != prop_key) zend_string_release(real_name);
				if (key_len > 2 && key[0] == '\0' && key[1] != '*') {
					zend_string_release(scope_str);
				}
				if (scoped_owned) zval_ptr_dtor(&local_scoped);
				zval_ptr_dtor(&obj_zval);
				RETURN_THROWS();
			}
			SEPARATE_ARRAY(scope_bucket);
			zend_string *name_to_use = real_name ? real_name : prop_key;
			Z_TRY_ADDREF_P(prop_val);
			zend_hash_update(Z_ARRVAL_P(scope_bucket), name_to_use, prop_val);

			if (real_name && real_name != prop_key) zend_string_release(real_name);
			/* Free scope_str only if we allocated it (private "\0ClassName\0" case) */
			if (key_len > 2 && key[0] == '\0' && key[1] != '*') {
				zend_string_release(scope_str);
			}
		} ZEND_HASH_FOREACH_END();
	}

	if (!scoped_props || !zend_hash_num_elements(scoped_props)) {
		if (scoped_owned) zval_ptr_dtor(&local_scoped);
		ZVAL_COPY_VALUE(return_value, &obj_zval);
		return;
	}

	zend_object *obj = Z_OBJ(obj_zval);
	zend_class_entry *obj_ce = obj->ce;

	/* One-time rebuild for stdClass — avoids the check inside the scope loop. */
	if (obj_ce == zend_standard_class_def && UNEXPECTED(!obj->properties)) {
		rebuild_object_properties_internal(obj);
	}

	zend_string *scope_name;
	zval *scope_props;
	ZEND_HASH_FOREACH_STR_KEY_VAL(scoped_props, scope_name, scope_props) {
		if (UNEXPECTED(!scope_name)) {
			zend_value_error("deepclone_hydrate(): Argument #2 ($vars) must have only string keys");
			if (scoped_owned) zval_ptr_dtor(&local_scoped);
			zval_ptr_dtor(&obj_zval);
			RETURN_THROWS();
		}
		if (UNEXPECTED(Z_TYPE_P(scope_props) != IS_ARRAY)) {
			zend_value_error("deepclone_hydrate(): Argument #2 ($vars) must have only array values, %s given for key \"%s\"",
				zend_zval_value_name(scope_props), ZSTR_VAL(scope_name));
			if (scoped_owned) zval_ptr_dtor(&local_scoped);
			zval_ptr_dtor(&obj_zval);
			RETURN_THROWS();
		}
		/* Footgun: NUL-prefixed scope key almost certainly means a missing DEEPCLONE_HYDRATE_MANGLED_VARS flag. */
		if (UNEXPECTED(ZSTR_VAL(scope_name)[0] == '\0')) {
			zend_value_error("deepclone_hydrate(): Argument #2 ($vars) contains a NUL-prefixed key — "
				"pass DEEPCLONE_HYDRATE_MANGLED_VARS in the $flags argument to interpret $vars as a flat mangled-key array");
			if (scoped_owned) zval_ptr_dtor(&local_scoped);
			zval_ptr_dtor(&obj_zval);
			RETURN_THROWS();
		}

		/* Resolve scope class entry — must be obj_ce, a parent class, or stdClass.
		 * No autoloader is triggered; interfaces are not valid scopes. */
		zend_class_entry *scope_ce = NULL;
		if (EXPECTED(zend_string_equals(scope_name, obj_ce->name))) {
			scope_ce = obj_ce;
		} else if (zend_string_equals(scope_name, ZEND_STANDARD_CLASS_DEF_PTR->name)) {
			/* stdClass scope = dynamic/public properties, fake_scope stays NULL */
		} else {
			zend_class_entry *p = obj_ce->parent;
			while (p) {
				if (zend_string_equals(scope_name, p->name)) {
					scope_ce = p;
					break;
				}
				p = p->parent;
			}
			if (UNEXPECTED(!scope_ce)) {
				zend_value_error("deepclone_hydrate(): Argument #2 ($vars) scope \"%s\" is not a parent of \"%s\"",
					ZSTR_VAL(scope_name), ZSTR_VAL(obj_ce->name));
				if (scoped_owned) zval_ptr_dtor(&local_scoped);
				zval_ptr_dtor(&obj_zval);
				RETURN_THROWS();
			}
		}

#if PHP_VERSION_ID >= 80500
		const zend_class_entry *old_scope = EG(fake_scope);
#else
		zend_class_entry *old_scope = EG(fake_scope);
#endif
		if (scope_ce) {
			EG(fake_scope) = scope_ce;
		}

		zend_ulong prop_idx;
		zend_string *prop_name;
		zval *prop_val;
		ZEND_HASH_FOREACH_KEY_VAL(Z_ARRVAL_P(scope_props), prop_idx, prop_name, prop_val) {
			/* Integer keys: coerce to string on dynamic property access, matching
			 * unserialize()'s permissiveness. Allocated name is released below. */
			bool prop_name_owned = false;
			if (!prop_name) {
				prop_name = zend_long_to_str((zend_long) prop_idx);
				prop_name_owned = true;
			}

			/* "\0" key = internal state for ArrayObject/ArrayIterator/SplObjectStorage */
			if (ZSTR_LEN(prop_name) == 1 && ZSTR_VAL(prop_name)[0] == '\0') {
				if (UNEXPECTED(Z_TYPE_P(prop_val) != IS_ARRAY)) {
					zend_value_error("deepclone_hydrate(): Argument #2 ($vars) scope \"%s\" special \"\\0\" value must be of type array, %s given",
						ZSTR_VAL(scope_name), zend_zval_value_name(prop_val));
					EG(fake_scope) = old_scope;
					if (scoped_owned) zval_ptr_dtor(&local_scoped);
					zval_ptr_dtor(&obj_zval);
					RETURN_THROWS();
				}

				if (instanceof_function(obj_ce, spl_ce_SplObjectStorage)) {
					/* [$obj1, $info1, $obj2, $info2, ...] → offsetSet(obj, info) */
					uint32_t count = zend_hash_num_elements(Z_ARRVAL_P(prop_val));
					for (uint32_t i = 0; i + 1 < count; i += 2) {
						zval *zkey = zend_hash_index_find(Z_ARRVAL_P(prop_val), i);
						zval *zinfo = zend_hash_index_find(Z_ARRVAL_P(prop_val), i + 1);
						if (!zkey || !zinfo) continue;
						zend_call_method_with_2_params(obj, obj_ce, NULL, "offsetset", NULL, zkey, zinfo);
						if (UNEXPECTED(EG(exception))) {
							EG(fake_scope) = old_scope;
							if (scoped_owned) zval_ptr_dtor(&local_scoped);
							zval_ptr_dtor(&obj_zval);
							RETURN_THROWS();
						}
					}
				} else if (instanceof_function(obj_ce, spl_ce_ArrayObject)
				        || instanceof_function(obj_ce, spl_ce_ArrayIterator)) {
					/* [$array, $flags?, $iteratorClass?] — call constructor */
					zend_function *ctor = obj_ce->constructor;
					if (ctor) {
						uint32_t argc = zend_hash_num_elements(Z_ARRVAL_P(prop_val));
						if (argc > 3) argc = 3;
						zval args[3];
						for (uint32_t a = 0; a < argc; a++) {
							zval *arg = zend_hash_index_find(Z_ARRVAL_P(prop_val), a);
							ZVAL_COPY_VALUE(&args[a], arg ? arg : &EG(uninitialized_zval));
						}
						zval retval;
						ZVAL_UNDEF(&retval);
						zend_call_known_function(ctor, obj, obj_ce, &retval, argc, args, NULL);
						zval_ptr_dtor(&retval);
						if (UNEXPECTED(EG(exception))) {
							EG(fake_scope) = old_scope;
							if (scoped_owned) zval_ptr_dtor(&local_scoped);
							zval_ptr_dtor(&obj_zval);
							RETURN_THROWS();
						}
					}
				}
				continue;
			}

			/* Default: the input's PHP & references are dropped (dereferenced) on
			 * write. Pass DEEPCLONE_HYDRATE_PRESERVE_REFS to keep the ref link,
			 * which matters for call sites that intentionally share a value slot
			 * between two properties or between a property and a caller-side var. */
			if (!(flags & DEEPCLONE_HYDRATE_PRESERVE_REFS)) {
				ZVAL_DEREF(prop_val);
			}

			if (obj_ce == zend_standard_class_def) {
				/* Matches unserialize(): NUL-in-middle names are stored as-is,
				 * NUL-prefix names are rejected by the engine on read. No
				 * pre-validation — the polyfill follows the same rule. */
				Z_TRY_ADDREF_P(prop_val);
				zend_hash_update(obj->properties, prop_name, prop_val);
			} else {
				/* Try direct property slot write first — declared property names
				 * are engine-interned and never contain NUL, so a successful
				 * lookup is a fast-path that skips all validation.
				 * Use _known_hash: prop_name comes from a HashTable iteration,
				 * so its hash is already populated. */
				zend_property_info *pi = NULL;
				if (scope_ce) {
					zval *zv = zend_hash_find_known_hash(&scope_ce->properties_info, prop_name);
					if (zv) pi = Z_PTR_P(zv);
				}
				if (dc_is_backed_declared_property(pi)) {
					bool ok = dc_write_backed_property(obj, pi, prop_name, prop_val, flags);
					if (UNEXPECTED(!ok)) {
						if (prop_name_owned) zend_string_release(prop_name);
						EG(fake_scope) = old_scope;
						if (scoped_owned) zval_ptr_dtor(&local_scoped);
						zval_ptr_dtor(&obj_zval);
						RETURN_THROWS();
					}
				} else {
					/* Fallback: dynamic property or unknown name. Matches
					 * unserialize(): the engine rejects NUL-prefix names with
					 * \Error and accepts NUL-in-middle as raw dynamic props. */
					zend_std_write_property(obj, prop_name, prop_val, NULL);
					if (UNEXPECTED(EG(exception))) {
						if (prop_name_owned) zend_string_release(prop_name);
						EG(fake_scope) = old_scope;
						if (scoped_owned) zval_ptr_dtor(&local_scoped);
						zval_ptr_dtor(&obj_zval);
						RETURN_THROWS();
					}
				}
			}
			if (prop_name_owned) zend_string_release(prop_name);
		} ZEND_HASH_FOREACH_END();

		EG(fake_scope) = old_scope;
	} ZEND_HASH_FOREACH_END();

	if (scoped_owned) zval_ptr_dtor(&local_scoped);
	ZVAL_COPY_VALUE(return_value, &obj_zval);
}

/* ── Module boilerplate ─────────────────────────────────────── */

/* Function arginfo and ext_functions[] are generated from deepclone.stub.php
 * into deepclone_arginfo.h — see the @generate-class-entries directive there. */

PHP_MINIT_FUNCTION(deepclone)
{
	dc_key_value       = zend_string_init_interned(ZEND_STRL("value"), 1);
	dc_key_classes     = zend_string_init_interned(ZEND_STRL("classes"), 1);
	dc_key_object_meta = zend_string_init_interned(ZEND_STRL("objectMeta"), 1);
	dc_key_prepared    = zend_string_init_interned(ZEND_STRL("prepared"), 1);
	dc_key_mask        = zend_string_init_interned(ZEND_STRL("mask"), 1);
	dc_key_properties  = zend_string_init_interned(ZEND_STRL("properties"), 1);
	dc_key_resolve     = zend_string_init_interned(ZEND_STRL("resolve"), 1);
	dc_key_states      = zend_string_init_interned(ZEND_STRL("states"), 1);
	dc_key_refs        = zend_string_init_interned(ZEND_STRL("refs"), 1);
	dc_key_ref_masks   = zend_string_init_interned(ZEND_STRL("refMasks"), 1);

	dc_str_trace                    = zend_string_init_interned(ZEND_STRL("trace"), 1);
	dc_str_error_trace_mangled      = zend_string_init_interned("\0Error\0trace", sizeof("\0Error\0trace") - 1, 1);
	dc_str_exception_trace_mangled  = zend_string_init_interned("\0Exception\0trace", sizeof("\0Exception\0trace") - 1, 1);
	dc_str_file_mangled             = zend_string_init_interned("\0*\0file", sizeof("\0*\0file") - 1, 1);
	dc_str_line_mangled             = zend_string_init_interned("\0*\0line", sizeof("\0*\0line") - 1, 1);

	/* ReflectionGenerator is PHPAPI in php_reflection.c but missing from the
	 * header. Module deps ensure ext/reflection MINIT runs before us, so the
	 * class table lookup is guaranteed to find it. */
	dc_ce_reflection_generator = zend_hash_str_find_ptr(CG(class_table),
		"reflectiongenerator", sizeof("reflectiongenerator") - 1);

	/* Register \DeepClone\NotInstantiableException and
	 * \DeepClone\ClassNotFoundException, both extending \InvalidArgumentException.
	 * The registration helpers are generated from deepclone.stub.php into
	 * deepclone_arginfo.h. */
	dc_ce_not_instantiable_exception =
		register_class_DeepClone_NotInstantiableException(spl_ce_InvalidArgumentException);
	dc_ce_class_not_found_exception =
		register_class_DeepClone_ClassNotFoundException(spl_ce_InvalidArgumentException);

	register_deepclone_symbols(module_number);

	return SUCCESS;
}

static const zend_module_dep deepclone_deps[] = {
	ZEND_MOD_REQUIRED("reflection")
	ZEND_MOD_REQUIRED("spl")
	ZEND_MOD_END
};

ZEND_DECLARE_MODULE_GLOBALS(deepclone)

static PHP_GINIT_FUNCTION(deepclone)
{
#if defined(COMPILE_DL_DEEPCLONE) && defined(ZTS)
	ZEND_TSRMLS_CACHE_UPDATE();
#endif
	zend_hash_init(&deepclone_globals->hydrate_cache, 8, NULL, NULL, 1);
	/* lazy_init_refl_cache holds zend_object* (request-scoped). Initialized
	 * lazily on first use in RINIT-equivalent flow; cleared in RSHUTDOWN. */
	memset(&deepclone_globals->lazy_init_refl_cache, 0, sizeof(HashTable));
}

static PHP_GSHUTDOWN_FUNCTION(deepclone)
{
	zend_hash_destroy(&deepclone_globals->hydrate_cache);
}

#if PHP_VERSION_ID >= 80400
static void dc_lazy_refl_cache_dtor(zval *zv)
{
	zend_object_release((zend_object *) Z_PTR_P(zv));
}
#endif

static PHP_RSHUTDOWN_FUNCTION(deepclone)
{
	HashTable *cache = &DC_G(lazy_init_refl_cache);
	if (cache->nTableSize) {
		zend_hash_destroy(cache);
		memset(cache, 0, sizeof(HashTable));
	}
	return SUCCESS;
}

PHP_MINFO_FUNCTION(deepclone)
{
	php_info_print_table_start();
	php_info_print_table_row(2, "deepclone support", "enabled");
	php_info_print_table_row(2, "deepclone version", PHP_DEEPCLONE_VERSION);
	php_info_print_table_end();
}

zend_module_entry deepclone_module_entry = {
	STANDARD_MODULE_HEADER_EX,
	NULL,
	deepclone_deps,
	"deepclone",
	ext_functions,
	PHP_MINIT(deepclone),
	NULL, /* MSHUTDOWN */
	NULL, /* RINIT */
	PHP_RSHUTDOWN(deepclone),
	PHP_MINFO(deepclone),
	PHP_DEEPCLONE_VERSION,
	PHP_MODULE_GLOBALS(deepclone),
	PHP_GINIT(deepclone),
	PHP_GSHUTDOWN(deepclone),
	NULL, /* post-deactivate */
	STANDARD_MODULE_PROPERTIES_EX
};

#ifdef COMPILE_DL_DEEPCLONE
# ifdef ZTS
ZEND_TSRMLS_CACHE_DEFINE()
# endif
ZEND_GET_MODULE(deepclone)
#endif
