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
 * [] = allow none, case-insensitive.
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
#include "Zend/zend_attributes.h"
#include "Zend/zend_interfaces.h"
#include "ext/spl/spl_iterators.h"
#include "ext/spl/spl_exceptions.h"

/* ext/reflection's class entries are PHPAPI but Debian's php-dev does not
 * ship ext/reflection/php_reflection.h. Forward-declare what we use; the
 * linker resolves the symbols against the loaded PHP binary at runtime. */
extern PHPAPI zend_class_entry *reflector_ptr;
extern PHPAPI zend_class_entry *reflection_type_ptr;
extern PHPAPI zend_class_entry *reflection_property_ptr;

/* PHPAPI helpers exposed by ext/reflection in PHP 8.6+. They encapsulate the
 * setRawValue / setRawValueWithoutLazyInitialization logic — including the
 * trampoline-based hook bypass and lazy-prop/realize handling — that we
 * previously had to either re-implement or delegate to via a userland
 * ReflectionProperty round-trip. */
#if PHP_VERSION_ID >= 80600
extern PHPAPI void zend_reflection_property_set_raw_value(
	zend_property_info *prop, zend_string *unmangled_name,
	void *cache_slot[3], const zend_class_entry *scope,
	zend_object *object, zval *value);
extern PHPAPI void zend_reflection_property_set_raw_value_without_lazy_initialization(
	zend_property_info *prop, zend_string *unmangled_name,
	void *cache_slot[3], const zend_class_entry *scope,
	zend_object *object, zval *value);
#endif

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
#define DEEPCLONE_HYDRATE_PRESERVE_REFS (1 << 2)
#define DEEPCLONE_HYDRATE_FLAGS_MASK \
	(DEEPCLONE_HYDRATE_CALL_HOOKS | DEEPCLONE_HYDRATE_NO_LAZY_INIT | DEEPCLONE_HYDRATE_PRESERVE_REFS)

/* IS_PROP_REINITABLE (readonly clone-with bookkeeping) landed in PHP 8.3.
 * On 8.2 there is no such flag; clearing 0 bits is a no-op. */
#ifndef IS_PROP_REINITABLE
# define IS_PROP_REINITABLE (0)
#endif

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
	bool           allow_named_closures; /* opt-in: encode closures over named callables by name */
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
	ctx->allow_named_closures = false;
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
	/* Non-static, non-virtual property with a real backing slot. ZEND_ACC_VIRTUAL
	 * already implies pi->offset == ZEND_VIRTUAL_PROPERTY_OFFSET, and
	 * IS_HOOKED_PROPERTY_OFFSET() is only meaningful on offsets returned by
	 * zend_get_property_offset() — not on the raw pi->offset. */
	return pi && !(pi->flags & (ZEND_ACC_STATIC | ZEND_ACC_VIRTUAL));
}

static zend_always_inline bool dc_is_std_scope_property(zend_property_info *pi)
{
	return pi
		&& !(pi->flags & ZEND_ACC_STATIC)
		&& (pi->flags & ZEND_ACC_PUBLIC)
		&& !(pi->flags & (ZEND_ACC_PROTECTED_SET | ZEND_ACC_PRIVATE_SET));
}

/* Find-or-create the per-name [id] sub-array inside a properties/resolve
 * [scope] table, normalizing numeric property names to integer keys exactly
 * as PHP arrays do. An object property named "999" lives in the object's
 * property table as a verbatim string, but the array-shaped payload must use
 * the integer key 999 so it matches the polyfill's (array)-cast output and
 * survives a var_export()/JSON round-trip (both re-normalize "999" → 999).
 * Non-canonical names like "007" are not integer keys and stay strings.
 *
 * Only dynamic property names can be numeric (a declared property can't be
 * named like an integer), so callers that emit declared-only names pass
 * may_be_numeric=false to skip the ZEND_HANDLE_NUMERIC() probe entirely. */
static zend_always_inline zval *dc_name_subarray(HashTable *scope_ht, zend_string *name, bool may_be_numeric)
{
	zend_ulong idx;
	zval *slot;
	if (may_be_numeric && ZEND_HANDLE_NUMERIC(name, idx)) {
		slot = zend_hash_index_find(scope_ht, idx);
		if (!slot) {
			zval new_ht;
			array_init_size(&new_ht, 1);
			slot = zend_hash_index_add_new(scope_ht, idx, &new_ht);
		}
	} else {
		slot = zend_hash_find_known_hash(scope_ht, name);
		if (!slot) {
			zval new_ht;
			array_init_size(&new_ht, 1);
			slot = zend_hash_add_new(scope_ht, name, &new_ht);
		}
	}
	return slot;
}

/* Numeric-name-aware lookup of an existing per-name sub-array (no insert),
 * used to re-find a bucket after a recursive walk may have grown the table.
 * Mirrors dc_name_subarray()'s key handling, including the may_be_numeric
 * opt-out for declared-only names. */
static zend_always_inline zval *dc_name_subarray_find(HashTable *scope_ht, zend_string *name, bool may_be_numeric)
{
	zend_ulong idx;
	if (may_be_numeric && ZEND_HANDLE_NUMERIC(name, idx)) {
		return zend_hash_index_find(scope_ht, idx);
	}
	return zend_hash_find_known_hash(scope_ht, name);
}

#if PHP_VERSION_ID >= 80400 && PHP_VERSION_ID < 80600
/* fn_proxy slot cached across calls — first invocation fills it via method
 * lookup; subsequent invocations reuse the resolved zend_function*. */
static zend_function *dc_set_raw_no_lazy_fn = NULL;

/* HashTable destructor for lazy_init_refl_cache (releases cached
 * ReflectionProperty instances). Defined later; forward-declared here. */
static void dc_lazy_refl_cache_dtor(zval *zv);

/* Pre-PHP-8.6 fallback: PHP 8.6 exposes zend_reflection_property_set_raw_value_
 * without_lazy_initialization() as PHPAPI, so the lazy-prop dance lives in one
 * place in ext/reflection. On 8.4/8.5 we delegate through a userland
 * ReflectionProperty round-trip — construct (cached per pi) + invoke
 * setRawValueWithoutLazyInitialization($obj, $value). */
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
	bool call_hooks = (flags & DEEPCLONE_HYDRATE_CALL_HOOKS) != 0;
#if PHP_VERSION_ID >= 80400
	bool no_lazy_init = (flags & DEEPCLONE_HYDRATE_NO_LAZY_INIT) != 0;

	/* Lazy objects: a direct slot write would bypass the engine's realization
	 * hook and leave the object in a half-initialized state. Route through
	 * zend_update_property_ex() which triggers realization on first write.
	 * DEEPCLONE_HYDRATE_NO_LAZY_INIT has its own opt-out fast path below. */
	if (!no_lazy_init && UNEXPECTED(!zend_lazy_object_initialized(obj))) {
		zend_update_property_ex(pi->ce, obj, name, value);
		return !EG(exception);
	}
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
	 * (no backing slot to "unset", and the set hook may handle null itself).
	 * Skip the shortcut on lazy objects — a direct slot write would bypass
	 * the lazy-props bookkeeping. On NO_LAZY_INIT + lazy we fall through to
	 * the Reflection-based path below, which enforces type semantics. */
	if (Z_TYPE_P(value) == IS_NULL
		&& ZEND_TYPE_IS_SET(pi->type)
		&& !ZEND_TYPE_ALLOW_NULL(pi->type)
		&& !DC_PROP_HAS_HOOKS(pi)
#if PHP_VERSION_ID >= 80400
		&& zend_lazy_object_initialized(obj)
#endif
	) {
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
		&& !ZEND_TYPE_HAS_LIST(pi->type)
		/* Only cast for types of the form `Enum` or `?Enum` — unions like
		 * `Enum|string|int` already accept the scalar literally, so casting
		 * it to an enum case would be surprising. */
		&& (ZEND_TYPE_PURE_MASK(pi->type) & ~MAY_BE_NULL) == 0)
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
# if PHP_VERSION_ID >= 80600
		zend_reflection_property_set_raw_value_without_lazy_initialization(
			pi, name, NULL, pi->ce, obj, value);
		bool ok = !EG(exception);
# else
		bool ok = dc_set_raw_value_without_lazy_init(obj, pi, name, value);
# endif
# if PHP_VERSION_ID >= 80100
		if (enum_holder_used) {
			zval_ptr_dtor(&enum_holder);
		}
# endif
		return ok;
	}
#endif

	if (!ZEND_TYPE_IS_SET(pi->type) && !DC_PROP_HAS_HOOKS(pi) && !call_hooks) {
		/* Move the old value out before running its destructor: a __destruct
		 * on the old value can legitimately read (or reassign) this same slot.
		 * Install the new value first so reentrant reads see a valid slot. */
		zval old;
		ZVAL_COPY_VALUE(&old, slot);
		ZVAL_COPY(slot, value);
		zval_ptr_dtor(&old);
	}
	else if (UNEXPECTED(Z_ISREF_P(value)) && !DC_PROP_HAS_HOOKS(pi)) {
		/* Binding a shared PHP &-reference to a typed declared property:
		 * zend_std_write_property() only accepts dereferenced values (debug
		 * builds assert on it), so mirror unserialize(): verify the current
		 * referenced value against the property type, install the reference
		 * itself in the slot, and record the property as a type source so
		 * later writes through the reference keep being type-checked. */
		if (UNEXPECTED(!zend_verify_prop_assignable_by_ref(pi, value, /* strict */ 1))) {
			return false;
		}
		zval old;
		ZVAL_COPY_VALUE(&old, slot);
		ZVAL_COPY(slot, value);
		Z_PROP_FLAG_P(slot) &= ~(IS_PROP_UNINIT | IS_PROP_REINITABLE);
		ZEND_REF_ADD_TYPE_SOURCE(Z_REF_P(value), pi);
		if (UNEXPECTED(Z_ISREF(old))) {
			/* The replaced reference was necessarily bound to this property
			 * (every path that stores a reference in a typed slot adds the
			 * type source); unbind it before releasing. */
			ZEND_REF_DEL_TYPE_SOURCE(Z_REF(old), pi);
		}
		zval_ptr_dtor(&old);
	}
#if PHP_VERSION_ID >= 80600
	else if (!call_hooks) {
		/* Default mode: setRawValue semantics (bypass set hook on hooked
		 * non-virtual, type-check on typed). One PHPAPI call replaces our
		 * old trampoline + zend_update_property_ex split. */
		zend_reflection_property_set_raw_value(pi, name, NULL, pi->ce, obj, value);
	}
#elif PHP_VERSION_ID >= 80400
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
 * LONG(1)=constexpr_closure, STRING("e")=enum, ARRAY=nested sub-mask. */
#define DC_MASK_OBJ_REF(m)           ZVAL_TRUE(m)
#define DC_MASK_HARD_REF(m)          ZVAL_FALSE(m)
#define DC_MASK_NAMED_CLOSURE(m)     ZVAL_LONG((m), 0)
#define DC_MASK_CONSTEXPR_CLOSURE(m) ZVAL_LONG((m), 1)

/* Test whether a mask slot carries a particular marker. */
#define DC_MASK_IS_OBJ_REF(m)           (Z_TYPE_P(m) == IS_TRUE)
#define DC_MASK_IS_HARD_REF(m)          (Z_TYPE_P(m) == IS_FALSE)
#define DC_MASK_IS_NAMED_CLOSURE(m)     (Z_TYPE_P(m) == IS_LONG && Z_LVAL_P(m) == 0)
#define DC_MASK_IS_CONSTEXPR_CLOSURE(m) (Z_TYPE_P(m) == IS_LONG && Z_LVAL_P(m) == 1)

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

	/* Force hash (mixed) storage up front. dc_copy_value on a reference
	 * stashes new_dst_slot in ref_entry->tree_pos; if the first insert here
	 * transitioned dst from packed to hash mode, the later zend_hash_add_new
	 * would free the packed storage and leave that tree_pos dangling. */
	zend_hash_real_init_mixed(Z_ARRVAL_P(dst));
	zend_hash_real_init_mixed(Z_ARRVAL_P(mask_dst));

	ZEND_HASH_FOREACH_KEY_VAL(src_ht, idx, key, src_val) {
		/* __serialize() may return the object's raw property table (e.g.
		 * Random\Randomizer before PHP 8.3), where declared properties are
		 * IS_INDIRECT slots into the object. Resolve them like the native
		 * serializer does, or the payload would retain pointers that dangle
		 * once the source object is released. */
		if (UNEXPECTED(Z_TYPE_P(src_val) == IS_INDIRECT)) {
			src_val = Z_INDIRECT_P(src_val);
			if (Z_TYPE_P(src_val) == IS_UNDEF) {
				continue;
			}
		}
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

/* ── Const-expr closure references ───────────────────────────
 *
 * PHP 8.5 allows anonymous static closures in constant expressions
 * (attribute arguments, class constants, property and parameter defaults).
 * Such a closure carries no state, so it is encoded as a reference to its
 * declaration site: [class, site, attrIndex|null, closureIndex, startLine]
 * with site one of "" (the class), "NAME" (constant or enum case), "$name"
 * (property), "name()" (method) or "name()#N" (parameter). Restoring
 * re-evaluates the addressed constant expression and picks the Nth closure
 * found by a depth-first walk (arrays in order, objects through their
 * array-cast properties), then verifies the declaration line still matches.
 */

typedef struct {
	const zend_function *needle; /* locate: the target function to identify */
	uint32_t       want_ord;    /* resolve: ordinal of the closure to extract */
	uint32_t       ord;         /* running count of closures walked at this site */
	bool           matched;
	uint32_t       matched_ord;
	zval           found;
	HashTable      seen;        /* visited non-closure objects (cycle guard) */
} dc_cexpr_walk;

/* Identity match between a closure's target and the one being located. User
 * functions (methods, global functions, anonymous closures) share their
 * opcodes across closure instances; internal functions have none, so they are
 * matched by name and scope (a NULL scope is a global function). */
static bool dc_func_matches(const zend_function *a, const zend_function *b)
{
	if (a->type != b->type) {
		return false;
	}
	if (a->type == ZEND_USER_FUNCTION) {
		return a->op_array.opcodes == b->op_array.opcodes;
	}
	return a->common.scope == b->common.scope
		&& a->common.function_name && b->common.function_name
		&& zend_string_equals(a->common.function_name, b->common.function_name);
}

static void dc_cexpr_walk_init(dc_cexpr_walk *w, const zend_function *needle, uint32_t want_ord)
{
	w->needle = needle;
	w->want_ord = want_ord;
	w->ord = 0;
	w->matched = false;
	w->matched_ord = 0;
	ZVAL_UNDEF(&w->found);
	zend_hash_init(&w->seen, 0, NULL, NULL, 0);
}

static void dc_cexpr_walk_dtor(dc_cexpr_walk *w)
{
	zend_hash_destroy(&w->seen);
}

static zend_always_inline bool dc_cexpr_walk_done(const dc_cexpr_walk *w)
{
	return w->needle ? w->matched : !Z_ISUNDEF(w->found);
}

/* Depth-first walk counting every Closure instance. The order must match the
 * polyfill's walk exactly or payloads stop being interchangeable. */
static void dc_cexpr_walk_zval(dc_cexpr_walk *w, zval *val)
{
	if (UNEXPECTED(dc_check_stack_limit())) {
		return;
	}
	ZVAL_DEREF(val);

	if (Z_TYPE_P(val) == IS_OBJECT) {
		if (Z_OBJCE_P(val) == zend_ce_closure) {
			if (w->needle) {
				const zend_function *f = zend_get_closure_method_def(Z_OBJ_P(val));
				if (!w->matched && dc_func_matches(f, w->needle)) {
					w->matched = true;
					w->matched_ord = w->ord;
				}
			} else if (w->ord == w->want_ord && Z_ISUNDEF(w->found)) {
				ZVAL_COPY(&w->found, val);
			}
			w->ord++;
			return;
		}
		if (!zend_hash_index_add_empty_element(&w->seen, Z_OBJ_HANDLE_P(val))) {
			return;
		}
		HashTable *props = zend_get_properties_for(val, ZEND_PROP_PURPOSE_ARRAY_CAST);
		if (props) {
			zval *v;
			ZEND_HASH_FOREACH_VAL(props, v) {
				/* Declared properties surface as INDIRECT slots */
				ZVAL_DEINDIRECT(v);
				if (Z_TYPE_P(v) == IS_UNDEF) {
					continue;
				}
				dc_cexpr_walk_zval(w, v);
				if (dc_cexpr_walk_done(w) || EG(exception)) {
					break;
				}
			} ZEND_HASH_FOREACH_END();
			zend_release_properties(props);
		}
		return;
	}

	if (Z_TYPE_P(val) == IS_ARRAY) {
		zval *v;
		ZEND_HASH_FOREACH_VAL(Z_ARRVAL_P(val), v) {
			dc_cexpr_walk_zval(w, v);
			if (dc_cexpr_walk_done(w) || EG(exception)) {
				break;
			}
		} ZEND_HASH_FOREACH_END();
	}
}

/* Evaluate the args of one attribute and walk them. Returns false when the
 * evaluation failed; with clear_failure the exception is swallowed and the
 * site skipped, mirroring the polyfill's per-site try/catch. */
static bool dc_cexpr_walk_attr(dc_cexpr_walk *w, zend_attribute *attr, zend_class_entry *scope, bool clear_failure)
{
	uint32_t argc = attr->argc;
	zval *vals = argc ? safe_emalloc(argc, sizeof(zval), 0) : NULL;
	uint32_t evaluated = 0;
	bool ok = true;

	for (; evaluated < argc; evaluated++) {
		if (FAILURE == zend_get_attribute_value(&vals[evaluated], attr, evaluated, scope)) {
			ok = false;
			if (clear_failure && EG(exception)) {
				zend_clear_exception();
			}
			break;
		}
	}
	for (uint32_t i = 0; ok && i < argc && !dc_cexpr_walk_done(w) && !EG(exception); i++) {
		dc_cexpr_walk_zval(w, &vals[i]);
	}
	for (uint32_t i = 0; i < evaluated; i++) {
		zval_ptr_dtor(&vals[i]);
	}
	if (vals) {
		efree(vals);
	}
	return ok;
}

/* Evaluate one const-expr zval (constant value, property or parameter
 * default) on a copy and walk the result. */
static bool dc_cexpr_walk_const(dc_cexpr_walk *w, zval *src, zend_class_entry *scope, bool clear_failure)
{
	zval v;
	ZVAL_COPY(&v, src);
	if (Z_TYPE(v) == IS_CONSTANT_AST && FAILURE == zval_update_constant_ex(&v, scope)) {
		zval_ptr_dtor(&v);
		if (clear_failure && EG(exception)) {
			zend_clear_exception();
		}
		return false;
	}
	if (!EG(exception)) {
		dc_cexpr_walk_zval(w, &v);
	}
	zval_ptr_dtor(&v);
	return true;
}

static zval *dc_cexpr_prop_default(zend_class_entry *ce, const zend_property_info *prop)
{
#ifdef ZEND_ACC_VIRTUAL
	if (prop->flags & ZEND_ACC_VIRTUAL) {
		return NULL;
	}
#endif
	if (prop->flags & ZEND_ACC_STATIC) {
		return ce->default_static_members_table ? &ce->default_static_members_table[prop->offset] : NULL;
	}
	return ce->default_properties_table ? &ce->default_properties_table[OBJ_PROP_TO_NUM(prop->offset)] : NULL;
}

static zval *dc_cexpr_param_default(const zend_op_array *op_array, uint32_t param)
{
	for (uint32_t i = 0; i < op_array->last; i++) {
		const zend_op *op = &op_array->opcodes[i];
		if (op->opcode == ZEND_RECV_INIT) {
			if (op->op1.num == param + 1) {
				return RT_CONSTANT(op, op->op2);
			}
		} else if (op->opcode != ZEND_RECV && op->opcode != ZEND_RECV_VARIADIC) {
			break;
		}
	}
	return NULL;
}

#if PHP_VERSION_ID >= 80500
/* Builds [class, site, attrIndex|null, ord, line]; takes ownership of site. */
static void dc_cexpr_payload(zval *dst, zend_class_entry *ce, zend_string *site, zend_long attr_index, uint32_t ord, uint32_t line)
{
	zval tmp;
	array_init_size(dst, 5);
	ZVAL_STR_COPY(&tmp, ce->name);
	zend_hash_index_add_new(Z_ARRVAL_P(dst), 0, &tmp);
	ZVAL_STR(&tmp, site);
	zend_hash_index_add_new(Z_ARRVAL_P(dst), 1, &tmp);
	if (attr_index < 0) {
		ZVAL_NULL(&tmp);
	} else {
		ZVAL_LONG(&tmp, attr_index);
	}
	zend_hash_index_add_new(Z_ARRVAL_P(dst), 2, &tmp);
	ZVAL_LONG(&tmp, (zend_long) ord);
	zend_hash_index_add_new(Z_ARRVAL_P(dst), 3, &tmp);
	ZVAL_LONG(&tmp, (zend_long) line);
	zend_hash_index_add_new(Z_ARRVAL_P(dst), 4, &tmp);
}

/* Walk one attribute list (entries matching `offset`) looking for the needle.
 * On match fills *attr_index (ordinal among same-offset entries) and *ord. */
static bool dc_cexpr_locate_in_attrs(HashTable *attributes, uint32_t offset, zend_class_entry *scope, const zend_function *needle, uint32_t *attr_index, uint32_t *ord)
{
	if (!attributes) {
		return false;
	}
	uint32_t idx = 0;
	zend_attribute *attr;
	ZEND_HASH_FOREACH_PTR(attributes, attr) {
		if (attr->offset != offset) {
			continue;
		}
		dc_cexpr_walk w;
		dc_cexpr_walk_init(&w, needle, 0);
		dc_cexpr_walk_attr(&w, attr, scope, true);
		bool matched = w.matched;
		*ord = w.matched_ord;
		dc_cexpr_walk_dtor(&w);
		if (UNEXPECTED(EG(exception))) {
			return false;
		}
		if (matched) {
			*attr_index = idx;
			return true;
		}
		idx++;
	} ZEND_HASH_FOREACH_END();
	return false;
}

static bool dc_cexpr_locate_in_value(zval *src, zend_class_entry *scope, const zend_function *needle, uint32_t *ord)
{
	dc_cexpr_walk w;
	dc_cexpr_walk_init(&w, needle, 0);
	dc_cexpr_walk_const(&w, src, scope, true);
	bool matched = w.matched;
	*ord = w.matched_ord;
	dc_cexpr_walk_dtor(&w);
	return matched && !EG(exception);
}

/* Try to express an anonymous closure as a reference to the constant
 * expression that declares it. Identity is exact: the closure's op_array
 * shares its opcodes with the op_array embedded in the declaring AST.
 * Sites are scanned in the same order the polyfill indexes them, so both
 * implementations produce identical payloads. Promoted properties are
 * skipped: their constructor parameter is the canonical surface. */
/* Locate `target` as a closure declared in the constant expressions of an
 * explicit class `ce`. For an anonymous closure ce is its own scope; for a
 * first-class callable it is the declaring class, which differs from the
 * target's scope on cross-class references. */
static bool dc_cexpr_locate_ce(const zend_function *target, zend_class_entry *ce, zval *payload)
{
	const zend_function *needle = target;
	/* Internal functions have no line; resolution computes 0 for them, so the
	 * staleness check matches. */
	uint32_t line = target->type == ZEND_USER_FUNCTION ? target->op_array.line_start : 0;
	uint32_t attr_index, ord;
	zend_string *name;

	if (!ce) {
		return false;
	}

	/* class attributes */
	if (dc_cexpr_locate_in_attrs(ce->attributes, 0, ce, needle, &attr_index, &ord)) {
		dc_cexpr_payload(payload, ce, ZSTR_EMPTY_ALLOC(), (zend_long) attr_index, ord, line);
		return true;
	}
	if (UNEXPECTED(EG(exception))) {
		return false;
	}

	/* class constants and enum cases: attributes, then the value */
	zend_class_constant *c;
	ZEND_HASH_FOREACH_STR_KEY_PTR(&ce->constants_table, name, c) {
		if (c->ce != ce) {
			continue;
		}
		if (dc_cexpr_locate_in_attrs(c->attributes, 0, ce, needle, &attr_index, &ord)) {
			dc_cexpr_payload(payload, ce, zend_string_copy(name), (zend_long) attr_index, ord, line);
			return true;
		}
		if (!EG(exception) && dc_cexpr_locate_in_value(&c->value, c->ce, needle, &ord)) {
			dc_cexpr_payload(payload, ce, zend_string_copy(name), -1, ord, line);
			return true;
		}
		if (UNEXPECTED(EG(exception))) {
			return false;
		}
	} ZEND_HASH_FOREACH_END();

	/* properties: attributes, then the default value */
	zend_property_info *prop;
	ZEND_HASH_FOREACH_STR_KEY_PTR(&ce->properties_info, name, prop) {
		if (prop->ce != ce || (prop->flags & ZEND_ACC_PROMOTED)) {
			continue;
		}
		if (dc_cexpr_locate_in_attrs(prop->attributes, 0, ce, needle, &attr_index, &ord)) {
			dc_cexpr_payload(payload, ce, zend_strpprintf(0, "$%s", ZSTR_VAL(name)), (zend_long) attr_index, ord, line);
			return true;
		}
		zval *def = dc_cexpr_prop_default(ce, prop);
		if (!EG(exception) && def && Z_TYPE_P(def) != IS_UNDEF && dc_cexpr_locate_in_value(def, prop->ce, needle, &ord)) {
			dc_cexpr_payload(payload, ce, zend_strpprintf(0, "$%s", ZSTR_VAL(name)), -1, ord, line);
			return true;
		}
		if (UNEXPECTED(EG(exception))) {
			return false;
		}
		/* property hooks: attributes, then per parameter attributes */
		if (prop->hooks) {
			for (uint32_t hk = 0; hk < ZEND_PROPERTY_HOOK_COUNT; hk++) {
				const zend_function *hfn = prop->hooks[hk];
				if (!hfn || hfn->type != ZEND_USER_FUNCTION) {
					continue;
				}
				const char *hname = hk == ZEND_PROPERTY_HOOK_GET ? "get" : "set";
				if (dc_cexpr_locate_in_attrs(hfn->op_array.attributes, 0, ce, needle, &attr_index, &ord)) {
					dc_cexpr_payload(payload, ce, zend_strpprintf(0, "$%s::%s()", ZSTR_VAL(name), hname), (zend_long) attr_index, ord, line);
					return true;
				}
				if (UNEXPECTED(EG(exception))) {
					return false;
				}
				uint32_t hook_params = hfn->common.num_args + ((hfn->common.fn_flags & ZEND_ACC_VARIADIC) ? 1 : 0);
				for (uint32_t pi = 0; pi < hook_params; pi++) {
					if (dc_cexpr_locate_in_attrs(hfn->op_array.attributes, pi + 1, ce, needle, &attr_index, &ord)) {
						dc_cexpr_payload(payload, ce, zend_strpprintf(0, "$%s::%s()#%u", ZSTR_VAL(name), hname, pi), (zend_long) attr_index, ord, line);
						return true;
					}
					if (UNEXPECTED(EG(exception))) {
						return false;
					}
				}
			}
		}
	} ZEND_HASH_FOREACH_END();

	/* methods: attributes, then per parameter: attributes, then the default */
	zend_function *fn;
	ZEND_HASH_FOREACH_PTR(&ce->function_table, fn) {
		if (fn->common.scope != ce || fn->type != ZEND_USER_FUNCTION) {
			continue;
		}
		const char *fname = ZSTR_VAL(fn->common.function_name);
		if (dc_cexpr_locate_in_attrs(fn->op_array.attributes, 0, ce, needle, &attr_index, &ord)) {
			dc_cexpr_payload(payload, ce, zend_strpprintf(0, "%s()", fname), (zend_long) attr_index, ord, line);
			return true;
		}
		if (UNEXPECTED(EG(exception))) {
			return false;
		}
		uint32_t num_params = fn->common.num_args + ((fn->common.fn_flags & ZEND_ACC_VARIADIC) ? 1 : 0);
		for (uint32_t pi = 0; pi < num_params; pi++) {
			if (dc_cexpr_locate_in_attrs(fn->op_array.attributes, pi + 1, ce, needle, &attr_index, &ord)) {
				dc_cexpr_payload(payload, ce, zend_strpprintf(0, "%s()#%u", fname, pi), (zend_long) attr_index, ord, line);
				return true;
			}
			zval *def = dc_cexpr_param_default(&fn->op_array, pi);
			if (!EG(exception) && def && dc_cexpr_locate_in_value(def, ce, needle, &ord)) {
				dc_cexpr_payload(payload, ce, zend_strpprintf(0, "%s()#%u", fname, pi), -1, ord, line);
				return true;
			}
			if (UNEXPECTED(EG(exception))) {
				return false;
			}
		}
	} ZEND_HASH_FOREACH_END();

	return false;
}

/* Try to express a closure as a reference to the constant expression that
 * declares it, deriving the declaring class from the closure's own scope.
 * Covers anonymous closures and first-class callables over a method of their
 * own declaring class. */
static bool dc_cexpr_locate(const zend_function *target, zval *payload)
{
	return dc_cexpr_locate_ce(target, target->common.scope, payload);
}

/* ── Cross-class first-class-callable provenance (PHP 8.5, experimental) ──
 *
 * A first-class callable declared in a constant expression of class A but
 * referencing a method of class B (e.g. #[When(Validators::check(...))]) has
 * no link back to A on its closure object: its scope is B. PHP 8.6 records the
 * declaring class as engine provenance (ReflectionFunction::getConstExprClass);
 * 8.5 does not. To recover it without that API, we instrument
 * ReflectionAttribute::getArguments() and ::newInstance() (the paths frameworks
 * use to read attribute metadata) and record, for every cross-class FCC they
 * produce, a name-keyed map from the target to its declaring class. It is built
 * lazily and consulted only when the scope-based locate above fails. */

/* zif_handler carries ZEND_FASTCALL, a distinct calling convention on Windows;
 * match it so the handler swaps type-check under MSVC. */
static zif_handler dc_orig_attr_get_arguments = NULL;
static zif_handler dc_orig_attr_new_instance = NULL;

/* Mirrors ext/reflection's private object layout so we can read a
 * ReflectionAttribute's declaring-class scope. Must track the engine structs;
 * validated against the build at hand. */
typedef struct {
	zval               obj;
	void              *ptr;
	zend_class_entry  *ce;
	int                ref_type;   /* reflection_type_t */
	zend_object        zo;
} dc_refl_object_layout;

typedef struct {
	HashTable         *attributes;
	zend_attribute    *data;
	zend_class_entry  *scope;
	zend_string       *filename;
	uint32_t           target;
} dc_attr_ref_layout;

static zend_class_entry *dc_attr_declaring_scope(zend_object *obj)
{
	dc_refl_object_layout *ro = (dc_refl_object_layout *)
		((char *) obj - offsetof(dc_refl_object_layout, zo));
	dc_attr_ref_layout *ar = (dc_attr_ref_layout *) ro->ptr;
	return ar ? ar->scope : NULL;
}

/* The index persists across requests (per worker), so it is keyed and valued
 * by NAMES, not pointers: op_arrays and class entries are recompiled and freed
 * every request (without opcache), so a pointer index would dangle. Names
 * survive that churn, and the declaring class is re-resolved (without
 * autoloading) and re-located at serialization time, so a stale entry simply
 * misses instead of mis-resolving. Both keys and values are persistent strings
 * because a persistent HashTable only addref's the keys it is given. */
static void dc_provenance_dtor(zval *zv)
{
	zend_string_release((zend_string *) Z_PTR_P(zv));
}

/* Lowercased "targetClass\0method" — a request-lived, non-interned key. A NULL
 * target class (a global function) uses an empty class part, which no real
 * class can collide with. */
static zend_string *dc_provenance_key(zend_class_entry *target_ce, zend_string *method)
{
	size_t cl = target_ce ? ZSTR_LEN(target_ce->name) : 0, ml = ZSTR_LEN(method);
	zend_string *key = zend_string_alloc(cl + 1 + ml, 0);
	if (target_ce) {
		zend_str_tolower_copy(ZSTR_VAL(key), ZSTR_VAL(target_ce->name), cl);
	}
	ZSTR_VAL(key)[cl] = '\0';
	zend_str_tolower_copy(ZSTR_VAL(key) + cl + 1, ZSTR_VAL(method), ml);
	ZSTR_VAL(key)[cl + 1 + ml] = '\0';
	return key;
}

static void dc_provenance_store(zend_class_entry *target_ce, zend_string *method, zend_class_entry *scope)
{
	HashTable *idx = &DC_G(attr_provenance);
	if (!idx->nTableSize) {
		zend_hash_init(idx, 8, NULL, dc_provenance_dtor, 1);
	}
	zend_string *key = dc_provenance_key(target_ce, method);
	/* First declaring site wins; every site for one target is equivalent. */
	if (!zend_hash_exists(idx, key)) {
		zend_string *pkey = zend_string_dup(key, 1);
		zend_string *pval = zend_string_init(ZSTR_VAL(scope->name), ZSTR_LEN(scope->name), 1);
		zend_hash_add_ptr(idx, pkey, pval);
		zend_string_release(pkey);   /* the table holds its own reference */
	}
	zend_string_release(key);
}

static zend_class_entry *dc_provenance_lookup(zend_class_entry *target_ce, zend_string *method)
{
	HashTable *idx = &DC_G(attr_provenance);
	if (!idx->nTableSize) {
		return NULL;
	}
	zend_string *key = dc_provenance_key(target_ce, method);
	zend_string *decl = zend_hash_find_ptr(idx, key);
	zend_string_release(key);
	if (!decl) {
		return NULL;
	}
	/* No autoload: serialization must not load classes as a side effect.
	 * Under opcache.preload the declaring class is resident across requests. */
	return zend_lookup_class_ex(decl, NULL, ZEND_FETCH_CLASS_NO_AUTOLOAD);
}

/* The class whose constant expression declares this first-class callable. On
 * PHP 8.6 the engine records it (zend_constexpr_closure_ref), so it is exact
 * and needs no capture; on 8.5 it comes from the ReflectionAttribute-captured
 * index. Either way it feeds the same site-based (5-element) reference, which
 * is interchangeable with the polyfill — unlike the engine-id form, whose fcc
 * line userland cannot reproduce. */
static zend_class_entry *dc_declaring_class(zval *src, const zend_function *func)
{
#if PHP_VERSION_ID >= 80600
	zend_class_entry *ce;
	uint32_t id, line;
	if (zend_constexpr_closure_ref(Z_OBJ_P(src), &ce, &id, &line) == SUCCESS) {
		return ce;
	}
#endif
	return func->common.function_name
		? dc_provenance_lookup(func->common.scope, func->common.function_name)
		: NULL;
}

/* Walk a value (a getArguments() argument, or a newInstance() attribute object
 * and its properties), recording every cross-class FCC against `scope`. The
 * `seen` set guards cycles: getArguments() values are acyclic constant
 * expressions, but a newInstance() object is built by an arbitrary attribute
 * constructor and may be cyclic. */
static void dc_index_closures_rec(HashTable *seen, zval *val, zend_class_entry *scope)
{
	if (UNEXPECTED(dc_check_stack_limit())) {
		return;
	}
	ZVAL_DEREF(val);

	if (Z_TYPE_P(val) == IS_OBJECT) {
		if (Z_OBJCE_P(val) == zend_ce_closure) {
			const zend_function *f = zend_get_closure_method_def(Z_OBJ_P(val));
			/* Capture first-class callables the scope-based locate cannot find:
			 * cross-class methods (target scope differs from `scope`) and global
			 * functions (no scope at all), internal or user. An FCC over a method
			 * of `scope` itself is already found by the scope-based locate, and
			 * anonymous closures are not fake closures. */
			if (f && (f->common.fn_flags & ZEND_ACC_FAKE_CLOSURE)
					&& f->common.function_name && f->common.scope != scope) {
				dc_provenance_store(f->common.scope, f->common.function_name, scope);
			}
			return;
		}
		if (!zend_hash_index_add_empty_element(seen, Z_OBJ_HANDLE_P(val))) {
			return;
		}
		HashTable *props = zend_get_properties_for(val, ZEND_PROP_PURPOSE_ARRAY_CAST);
		if (props) {
			zval *v;
			ZEND_HASH_FOREACH_VAL(props, v) {
				ZVAL_DEINDIRECT(v);
				if (Z_TYPE_P(v) != IS_UNDEF) {
					dc_index_closures_rec(seen, v, scope);
				}
			} ZEND_HASH_FOREACH_END();
			zend_release_properties(props);
		}
		return;
	}

	if (Z_TYPE_P(val) == IS_ARRAY) {
		zval *v;
		ZEND_HASH_FOREACH_VAL(Z_ARRVAL_P(val), v) {
			dc_index_closures_rec(seen, v, scope);
		} ZEND_HASH_FOREACH_END();
	}
}

/* Common tail of both hooks: index the cross-class FCCs reachable from `val`
 * against the declaring class of the ReflectionAttribute `this`. */
static void dc_index_attr_closures(zval *this_zv, zval *val)
{
	if (!DC_G(capture_attribute_closures) || EG(exception)
			|| Z_TYPE_P(this_zv) != IS_OBJECT) {
		return;
	}
	zend_class_entry *scope = dc_attr_declaring_scope(Z_OBJ_P(this_zv));
	if (!scope) {
		return;
	}
	HashTable seen;
	zend_hash_init(&seen, 8, NULL, NULL, 0);
	dc_index_closures_rec(&seen, val, scope);
	zend_hash_destroy(&seen);
}

/* Instrumented ReflectionAttribute::getArguments(): the FCCs are the returned
 * argument values. */
static void ZEND_FASTCALL dc_attr_get_arguments_wrapper(INTERNAL_FUNCTION_PARAMETERS)
{
	dc_orig_attr_get_arguments(INTERNAL_FUNCTION_PARAM_PASSTHRU);
	if (Z_TYPE_P(return_value) == IS_ARRAY) {
		dc_index_attr_closures(ZEND_THIS, return_value);
	}
}

/* Instrumented ReflectionAttribute::newInstance(): the FCCs are properties of
 * the returned attribute instance. */
static void ZEND_FASTCALL dc_attr_new_instance_wrapper(INTERNAL_FUNCTION_PARAMETERS)
{
	dc_orig_attr_new_instance(INTERNAL_FUNCTION_PARAM_PASSTHRU);
	if (Z_TYPE_P(return_value) == IS_OBJECT) {
		dc_index_attr_closures(ZEND_THIS, return_value);
	}
}
#endif /* PHP_VERSION_ID >= 80500 */

/* deepclone_from_array() counterpart for engine-id references [class, id,
 * line], emitted on PHP >= 8.6: the id is the engine's canonical per-class
 * const-expr closure id (see Closure::fromConstExpr()). */
static void dc_cexpr_resolve_id(HashTable *ht, HashTable *allowed_set, zval *retval)
{
	zval *zclass = zend_hash_index_find(ht, 0);
	zval *zid = zend_hash_index_find(ht, 1);
	zval *zline = zend_hash_index_find(ht, 2);
	if (!zclass || !zid || !zline || zend_hash_num_elements(ht) != 3) {
		zend_value_error("deepclone_from_array(): malformed payload, const-expr-closure value must have 3 elements");
		return;
	}
	ZVAL_DEREF(zclass);
	ZVAL_DEREF(zid);
	ZVAL_DEREF(zline);
	if (Z_TYPE_P(zclass) != IS_STRING) {
		zend_value_error("deepclone_from_array(): malformed payload, const-expr-closure class name must be of type string, %s given", zend_zval_value_name(zclass));
		return;
	}
	if (Z_TYPE_P(zline) != IS_LONG) {
		zend_value_error("deepclone_from_array(): malformed payload, const-expr-closure line must be of type int, %s given", zend_zval_value_name(zline));
		return;
	}

	/* Gate before zend_lookup_class(): the payload must not be able to
	 * autoload, let alone evaluate, classes outside the allow-list. */
	if (!dc_class_allowed(allowed_set, Z_STR_P(zclass))) {
		zend_value_error("deepclone_from_array(): class \"%s\" is not allowed", Z_STRVAL_P(zclass));
		return;
	}

#if PHP_VERSION_ID >= 80600
	zend_class_entry *ce = zend_lookup_class(Z_STR_P(zclass));
	if (!ce) {
		zend_value_error("deepclone_from_array(): malformed payload, const-expr-closure references unknown class \"%s\"", Z_STRVAL_P(zclass));
		return;
	}

	zend_ast *site = zend_constexpr_closure_site_by_id(ce, Z_LVAL_P(zid));
	if (!site) {
		zend_value_error("deepclone_from_array(): malformed payload, const-expr-closure references unknown closure id " ZEND_LONG_FMT " in class \"%s\"", Z_LVAL_P(zid), ZSTR_VAL(ce->name));
		return;
	}
	if (site->kind != ZEND_AST_OP_ARRAY) {
		zend_value_error("deepclone_from_array(): malformed payload, const-expr-closure references a first-class callable site");
		return;
	}

	zend_op_array *op = zend_ast_get_op_array(site)->op_array;
	if (Z_LVAL_P(zline) != (zend_long) op->line_start) {
		zend_value_error("deepclone_from_array(): stale payload, const-expr-closure moved from line " ZEND_LONG_FMT " to line %u", Z_LVAL_P(zline), op->line_start);
		return;
	}

	zend_create_closure(retval, (zend_function *) op, ce, ce, NULL);
#else
	zend_value_error("deepclone_from_array(): const-expr-closure payload was created on PHP 8.6 or later and cannot be resolved on PHP %s", PHP_VERSION);
#endif
}

/* deepclone_from_array() counterpart: resolve a declaration-site reference
 * back to a live Closure. */
static void dc_cexpr_resolve(zval *value, HashTable *allowed_set, zval *retval)
{
	if (Z_TYPE_P(value) != IS_ARRAY) {
		zend_value_error("deepclone_from_array(): malformed payload, const-expr-closure value must be of type array, %s given", zend_zval_value_name(value));
		return;
	}
	HashTable *ht = Z_ARRVAL_P(value);

	zval *zid = zend_hash_index_find(ht, 1);
	if (zid) {
		ZVAL_DEREF(zid);
	}
	if (zid && Z_TYPE_P(zid) == IS_LONG) {
		/* The type of element 1 (int id vs string site) discriminates
		 * engine-id references from site-based ones. */
		dc_cexpr_resolve_id(ht, allowed_set, retval);
		return;
	}

	zval *zclass = zend_hash_index_find(ht, 0);
	zval *zsite = zend_hash_index_find(ht, 1);
	zval *zattr = zend_hash_index_find(ht, 2);
	zval *zord = zend_hash_index_find(ht, 3);
	zval *zline = zend_hash_index_find(ht, 4);
	if (!zclass || !zsite || !zattr || !zord || !zline || zend_hash_num_elements(ht) != 5) {
		zend_value_error("deepclone_from_array(): malformed payload, const-expr-closure value must have 5 elements");
		return;
	}
	ZVAL_DEREF(zclass);
	ZVAL_DEREF(zsite);
	ZVAL_DEREF(zattr);
	ZVAL_DEREF(zord);
	ZVAL_DEREF(zline);
	if (Z_TYPE_P(zclass) != IS_STRING) {
		zend_value_error("deepclone_from_array(): malformed payload, const-expr-closure class name must be of type string, %s given", zend_zval_value_name(zclass));
		return;
	}
	if (Z_TYPE_P(zsite) != IS_STRING) {
		zend_value_error("deepclone_from_array(): malformed payload, const-expr-closure site must be of type string, %s given", zend_zval_value_name(zsite));
		return;
	}
	if (Z_TYPE_P(zattr) != IS_NULL && Z_TYPE_P(zattr) != IS_LONG) {
		zend_value_error("deepclone_from_array(): malformed payload, const-expr-closure attribute index must be of type int or null, %s given", zend_zval_value_name(zattr));
		return;
	}
	if (Z_TYPE_P(zord) != IS_LONG) {
		zend_value_error("deepclone_from_array(): malformed payload, const-expr-closure closure index must be of type int, %s given", zend_zval_value_name(zord));
		return;
	}
	if (Z_TYPE_P(zline) != IS_LONG) {
		zend_value_error("deepclone_from_array(): malformed payload, const-expr-closure line must be of type int, %s given", zend_zval_value_name(zline));
		return;
	}

	/* Gate before zend_lookup_class(): the payload must not be able to
	 * autoload, let alone evaluate, classes outside the allow-list. */
	if (!dc_class_allowed(allowed_set, Z_STR_P(zclass))) {
		zend_value_error("deepclone_from_array(): class \"%s\" is not allowed", Z_STRVAL_P(zclass));
		return;
	}

	zend_class_entry *ce = zend_lookup_class(Z_STR_P(zclass));
	if (!ce) {
		zend_value_error("deepclone_from_array(): malformed payload, const-expr-closure references unknown class \"%s\"", Z_STRVAL_P(zclass));
		return;
	}

	const char *site = Z_STRVAL_P(zsite);
	size_t site_len = Z_STRLEN_P(zsite);
	zend_long attr_index = Z_TYPE_P(zattr) == IS_LONG ? Z_LVAL_P(zattr) : -1;
	bool attr_site = Z_TYPE_P(zattr) == IS_LONG;
	zend_long want_ord = Z_LVAL_P(zord);
	zend_long line = Z_LVAL_P(zline);

	if (want_ord < 0 || want_ord > UINT32_MAX) {
		zend_value_error("deepclone_from_array(): malformed payload, const-expr-closure references unknown closure index " ZEND_LONG_FMT, want_ord);
		return;
	}

	/* Resolve the site to a target: its attribute list + evaluation scope,
	 * and for value sites the const-expr source zval. */
	HashTable *attributes = NULL;
	uint32_t attr_offset = 0;
	zend_class_entry *scope = ce;
	zval *const_src = NULL;
	bool has_value_site = false;

	if (0 == site_len) {
		attributes = ce->attributes;
	} else if ('$' == site[0]) {
		size_t sep = 0;
		for (size_t i = 1; i + 1 < site_len; i++) {
			if (':' == site[i] && ':' == site[i + 1]) {
				sep = i;
				break;
			}
		}
		if (sep) {
#if PHP_VERSION_ID < 80400
			zend_value_error("deepclone_from_array(): malformed payload, const-expr-closure references unknown hook \"%s\"", site);
			return;
#else
			/* property hook: "$prop::get()", "$prop::set()#N" */
			const zend_function *hfn = NULL;
			size_t spec_len = site_len - sep - 2;
			const char *spec = site + sep + 2;
			uint64_t param = 0;
			bool has_param = false;
			bool valid = true;
			if (spec_len > 5 && ')' == spec[4] && '#' == spec[5]) {
				has_param = true;
				valid = spec_len > 6 && ('0' != spec[6] || 7 == spec_len);
				for (size_t i = 6; valid && i < spec_len; i++) {
					valid = spec[i] >= '0' && spec[i] <= '9' && (param = param * 10 + (spec[i] - '0')) <= UINT32_MAX;
				}
				spec_len = 5;
			}
			uint32_t hook_kind = ZEND_PROPERTY_HOOK_COUNT;
			if (valid && 5 == spec_len && 0 == memcmp(spec + 3, "()", 2)) {
				if (0 == memcmp(spec, "get", 3)) {
					hook_kind = ZEND_PROPERTY_HOOK_GET;
				} else if (0 == memcmp(spec, "set", 3)) {
					hook_kind = ZEND_PROPERTY_HOOK_SET;
				}
			}
			if (hook_kind < ZEND_PROPERTY_HOOK_COUNT) {
				zend_property_info *prop = zend_hash_str_find_ptr(&ce->properties_info, site + 1, sep - 1);
				if (prop && prop->hooks && prop->hooks[hook_kind] && prop->hooks[hook_kind]->type == ZEND_USER_FUNCTION) {
					hfn = prop->hooks[hook_kind];
				}
			}
			uint32_t hook_params = hfn ? hfn->common.num_args + ((hfn->common.fn_flags & ZEND_ACC_VARIADIC) ? 1 : 0) : 0;
			if (!hfn || (has_param && param >= hook_params)) {
				zend_value_error("deepclone_from_array(): malformed payload, const-expr-closure references unknown hook \"%s\"", site);
				return;
			}
			attributes = hfn->op_array.attributes;
			scope = hfn->common.scope;
			if (has_param) {
				attr_offset = (uint32_t) param + 1;
				const_src = dc_cexpr_param_default(&hfn->op_array, (uint32_t) param);
				has_value_site = true;
			}
#endif
		} else {
			zend_property_info *prop = zend_hash_str_find_ptr(&ce->properties_info, site + 1, site_len - 1);
			if (!prop) {
				zend_value_error("deepclone_from_array(): malformed payload, const-expr-closure references unknown property \"%s\"", site);
				return;
			}
			attributes = prop->attributes;
			scope = prop->ce;
			const_src = dc_cexpr_prop_default(ce, prop);
			has_value_site = true;
		}
	} else {
		/* "name()#N" parameter site? */
		size_t paren = 0;
		for (size_t i = 1; i + 1 < site_len; i++) {
			if (')' == site[i] && '#' == site[i + 1]) {
				paren = i;
				break;
			}
		}
		if (paren) {
			zend_function *fn = NULL;
			uint64_t param = 0;
			bool valid = paren >= 2 && '(' == site[paren - 1] && paren + 2 < site_len
				&& ('0' != site[paren + 2] || paren + 3 == site_len);
			for (size_t i = paren + 2; valid && i < site_len; i++) {
				valid = site[i] >= '0' && site[i] <= '9' && (param = param * 10 + (site[i] - '0')) <= UINT32_MAX;
			}
			if (valid) {
				zend_string *mname = zend_string_init(site, paren - 1, 0);
				fn = zend_hash_find_ptr_lc(&ce->function_table, mname);
				zend_string_release(mname);
			}
			uint32_t num_params = fn && fn->type == ZEND_USER_FUNCTION
				? fn->common.num_args + ((fn->common.fn_flags & ZEND_ACC_VARIADIC) ? 1 : 0) : 0;
			if (!valid || !fn || param >= num_params) {
				zend_value_error("deepclone_from_array(): malformed payload, const-expr-closure references unknown parameter \"%s\"", site);
				return;
			}
			attributes = fn->op_array.attributes;
			attr_offset = (uint32_t) param + 1;
			scope = fn->common.scope;
			const_src = dc_cexpr_param_default(&fn->op_array, (uint32_t) param);
			has_value_site = true;
		} else if (site_len >= 3 && '(' == site[site_len - 2] && ')' == site[site_len - 1]) {
			zend_string *mname = zend_string_init(site, site_len - 2, 0);
			zend_function *fn = zend_hash_find_ptr_lc(&ce->function_table, mname);
			zend_string_release(mname);
			if (!fn || fn->type != ZEND_USER_FUNCTION) {
				zend_value_error("deepclone_from_array(): malformed payload, const-expr-closure references unknown method \"%s\"", site);
				return;
			}
			attributes = fn->op_array.attributes;
			scope = fn->common.scope;
		} else {
			zend_class_constant *c = zend_hash_find_ptr(&ce->constants_table, Z_STR_P(zsite));
			if (!c) {
				zend_value_error("deepclone_from_array(): malformed payload, const-expr-closure references unknown constant \"%s\"", site);
				return;
			}
			attributes = c->attributes;
			scope = c->ce;
			const_src = &c->value;
			has_value_site = true;
		}
	}

	dc_cexpr_walk w;
	dc_cexpr_walk_init(&w, NULL, (uint32_t) want_ord);

	if (attr_site) {
		zend_attribute *attr = NULL;
		if (attr_index >= 0 && attributes) {
			uint32_t idx = 0;
			zend_attribute *a;
			ZEND_HASH_FOREACH_PTR(attributes, a) {
				if (a->offset != attr_offset) {
					continue;
				}
				if (idx == (uint32_t) attr_index) {
					attr = a;
					break;
				}
				idx++;
			} ZEND_HASH_FOREACH_END();
		}
		if (!attr) {
			dc_cexpr_walk_dtor(&w);
			zend_value_error("deepclone_from_array(): malformed payload, const-expr-closure references unknown attribute index " ZEND_LONG_FMT, attr_index);
			return;
		}
		if (!dc_cexpr_walk_attr(&w, attr, scope, false) || EG(exception)) {
			dc_cexpr_walk_dtor(&w);
			zval_ptr_dtor(&w.found);
			zend_value_error("deepclone_from_array(): malformed payload, const-expr-closure evaluation failed for site \"%s\"", site);
			return;
		}
	} else if (!has_value_site) {
		dc_cexpr_walk_dtor(&w);
		zend_value_error("deepclone_from_array(): malformed payload, const-expr-closure attribute index is required for site \"%s\"", site);
		return;
	} else if (const_src && Z_TYPE_P(const_src) != IS_UNDEF
			&& (!dc_cexpr_walk_const(&w, const_src, scope, false) || EG(exception))) {
		dc_cexpr_walk_dtor(&w);
		zval_ptr_dtor(&w.found);
		zend_value_error("deepclone_from_array(): malformed payload, const-expr-closure evaluation failed for site \"%s\"", site);
		return;
	}

	dc_cexpr_walk_dtor(&w);
	if (Z_ISUNDEF(w.found)) {
		zend_value_error("deepclone_from_array(): malformed payload, const-expr-closure references unknown closure index " ZEND_LONG_FMT, want_ord);
		return;
	}

	const zend_function *f = zend_get_closure_method_def(Z_OBJ(w.found));
	uint32_t found_line = f->type == ZEND_USER_FUNCTION ? f->op_array.line_start : 0;
	if (line != (zend_long) found_line) {
		zval_ptr_dtor(&w.found);
		zend_value_error("deepclone_from_array(): stale payload, const-expr-closure moved from line " ZEND_LONG_FMT " to line %u", line, found_line);
		return;
	}

	ZVAL_COPY_VALUE(retval, &w.found);
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
		/* Always do the pool lookup — an object with refcount==1 can still be
		 * reached twice if a SHARED parent array is walked from multiple paths.
		 * The earlier "skip on refcount==1" optimization tripped add_new() on
		 * the second visit, since pool state persists across walks. */
		zval *pooled = zend_hash_index_find(&ctx->object_pool, handle);
		if (UNEXPECTED(pooled != NULL)) {
			ctx->objects_count++;
			dc_pool_entry *entry = (dc_pool_entry *)Z_PTR_P(pooled);
			ZVAL_LONG(dst, entry->id);
			DC_MASK_OBJ_REF(mask_dst);
			goto handle_value;
		}
	}

	/* ── Closures ───────────────────────────────── */
	if (Z_OBJCE_P(src) == zend_ce_closure) {
		const zend_function *func = zend_get_closure_method_def(Z_OBJ_P(src));

#if PHP_VERSION_ID >= 80500
		/* Const-expr declaration-site reference. This covers anonymous static
		 * closures and first-class callables over a method of their own
		 * declaring class (e.g. #[When(self::isStrict(...))]). It is attempted
		 * before the by-name encoding so that such closures serialize as a
		 * declaration-site reference — resolvable only to what the class
		 * itself declares — and therefore round-trip without requiring the
		 * allow_named_closures opt-in. The allow-list is checked first so that
		 * disallowing Closure is reported before any const-expr of the scope
		 * class is evaluated. */
		if (func && func->type == ZEND_USER_FUNCTION && func->common.scope) {
			zval *this_ptr = zend_get_closure_this_ptr(src);
			if (!this_ptr || Z_TYPE_P(this_ptr) != IS_OBJECT) {
				if (!dc_class_allowed(ctx->allowed_ht, zend_ce_closure->name)) {
					zend_value_error("deepclone_to_array(): class \"Closure\" is not allowed");
					return;
				}
#if PHP_VERSION_ID >= 80600
				/* The engine assigns a canonical per-class id to anonymous closures
				 * declared in attribute arguments and parameter default values; prefer
				 * it to the site-based reference below. First-class callables are
				 * excluded: their engine id resolves to an fcc site the decode side
				 * cannot recreate, so they keep the site-based and by-name paths.
				 * Closures in class constant values and property defaults have no id
				 * and also fall through. */
				if (!(func->common.fn_flags & ZEND_ACC_FAKE_CLOSURE)) {
					zend_class_entry *site_ce;
					uint32_t cexpr_id, cexpr_line;
					if (zend_constexpr_closure_ref(Z_OBJ_P(src), &site_ce, &cexpr_id, &cexpr_line) == SUCCESS) {
						zval tmp;
						array_init_size(dst, 3);
						ZVAL_STR_COPY(&tmp, site_ce->name);
						zend_hash_index_add_new(Z_ARRVAL_P(dst), 0, &tmp);
						ZVAL_LONG(&tmp, (zend_long) cexpr_id);
						zend_hash_index_add_new(Z_ARRVAL_P(dst), 1, &tmp);
						ZVAL_LONG(&tmp, (zend_long) cexpr_line);
						zend_hash_index_add_new(Z_ARRVAL_P(dst), 2, &tmp);
						DC_MASK_CONSTEXPR_CLOSURE(mask_dst);
						goto handle_value;
					}
				}
#endif
				zval payload;
				ZVAL_UNDEF(&payload);
				if (dc_cexpr_locate(func, &payload)) {
					ZVAL_COPY_VALUE(dst, &payload);
					DC_MASK_CONSTEXPR_CLOSURE(mask_dst);
					goto handle_value;
				}
				if (UNEXPECTED(EG(exception))) {
					return;
				}
				/* Cross-class first-class callable: the declaring class is not
				 * the closure's scope, so the locate above (which walks the
				 * scope) misses. On 8.5 there is no engine provenance; fall back
				 * to a declaring class captured from ReflectionAttribute, if
				 * any, and locate the site there. */
				if (func->common.function_name) {
					zend_class_entry *decl = dc_declaring_class(src, func);
					if (decl && decl != func->common.scope && dc_cexpr_locate_ce(func, decl, &payload)) {
						ZVAL_COPY_VALUE(dst, &payload);
						DC_MASK_CONSTEXPR_CLOSURE(mask_dst);
						goto handle_value;
					}
					if (UNEXPECTED(EG(exception))) {
						return;
					}
				}
			}
		}

		/* Global-function first-class callable (no scope, internal or user):
		 * the declaring class comes from the engine (8.6) or captured
		 * provenance (8.5). Same
		 * declaration-site reference and Closure gating as above; unresolved
		 * ones fall through to the by-name path. */
		if (func && (func->common.fn_flags & ZEND_ACC_FAKE_CLOSURE)
				&& !func->common.scope && func->common.function_name) {
			zval *this_ptr = zend_get_closure_this_ptr(src);
			if (!this_ptr || Z_TYPE_P(this_ptr) != IS_OBJECT) {
				zend_class_entry *decl = dc_declaring_class(src, func);
				if (decl) {
					if (!dc_class_allowed(ctx->allowed_ht, zend_ce_closure->name)) {
						zend_value_error("deepclone_to_array(): class \"Closure\" is not allowed");
						return;
					}
					zval payload;
					ZVAL_UNDEF(&payload);
					if (dc_cexpr_locate_ce(func, decl, &payload)) {
						ZVAL_COPY_VALUE(dst, &payload);
						DC_MASK_CONSTEXPR_CLOSURE(mask_dst);
						goto handle_value;
					}
					if (UNEXPECTED(EG(exception))) {
						return;
					}
				}
			}
		}
#endif

		/* Named closure: a first-class callable that is not addressable as a
		 * declaration-site reference (one created at runtime, or whose target
		 * lives outside its declaring class, or over an internal/global
		 * function). Encoding it stores the callable by name, which lets
		 * deepclone_from_array() mint a Closure over any function or method of
		 * that name — including internal functions such as system(). It is
		 * therefore gated behind the allow_named_closures opt-in, which both
		 * ends must enable. */
		if (func && (func->common.fn_flags & ZEND_ACC_FAKE_CLOSURE)) {
			if (!ctx->allow_named_closures) {
				zend_value_error("deepclone_to_array(): serializing a closure over the named callable \"%s\" requires enabling the \"allow_named_closures\" option; do it only if you trust the input", ZSTR_VAL(func->common.function_name));
				return;
			}
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
		/* Other closure (runtime anonymous, arrow fn) — fall through to
		 * regular object handling, which refuses it as non-instantiable. */
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
	/* Whether this object's property names can include numeric ones. Only
	 * dynamic properties can be named like an integer, so the declared-only
	 * fast path below clears this and the transpose skips the per-name
	 * numeric-key probe entirely. Declared at function scope (not mid-body)
	 * so the goto paths into build_scoped_props/prepare_props can't bypass
	 * its initializer. */
	bool may_have_numeric_names = true;

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
		/* Declared properties only — none can be numeric. */
		may_have_numeric_names = false;
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
			bool scope_name_owned = false;

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
				scope_name_owned = true;
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

			/* Add to scoped properties. The addref pairs with the hash's
			 * own ref — scope_name is either interned (release is a no-op)
			 * or owned here (release at next_prop via scope_name_owned). */
			{
				zval *scope_ht = zend_hash_find(Z_ARRVAL(props_zval), scope_name);
				if (!scope_ht) {
					zval new_ht;
					array_init(&new_ht);
					zend_string_addref(scope_name);
					scope_ht = zend_hash_add_new(Z_ARRVAL(props_zval), scope_name, &new_ht);
				}
				Z_TRY_ADDREF_P(arr_val);
				zend_hash_add_new(Z_ARRVAL_P(scope_ht), prop_name, arr_val);
			}

next_prop:
			if (prop_name_owned) {
				zend_string_release(prop_name);
			}
			if (scope_name_owned) {
				zend_string_release(scope_name);
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
				zval *out_name = dc_name_subarray(Z_ARRVAL_P(out_scope), name, may_have_numeric_names);

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
				out_name = dc_name_subarray_find(Z_ARRVAL_P(out_scope), name, may_have_numeric_names);
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
					zval *out_rname = dc_name_subarray(Z_ARRVAL_P(out_rscope), name, may_have_numeric_names);
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
	bool allow_named_closures = false;

	ZEND_PARSE_PARAMETERS_START(1, 3)
		Z_PARAM_ZVAL(value)
		Z_PARAM_OPTIONAL
		Z_PARAM_ARRAY_HT_OR_NULL(allowed_ht)
		Z_PARAM_BOOL(allow_named_closures)
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
	ctx.allow_named_closures = allow_named_closures;
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
 *   1      const-expr closure   →  re-evaluate its declaration site
 *   'e'    UnitEnum             →  resolve "Class::Case" string
 *   array  nested mask          →  recurse into the array's elements
 *   other  no marker            →  copy value as-is
 */
static void dc_resolve(zval *value, zval *mask, zval *objects, uint32_t num_objects, HashTable *refs, HashTable *allowed_set, zval *retval)
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
			if (UNEXPECTED(Z_TYPE_P(target) != IS_OBJECT)) {
				/* Object slots are filled before any value resolution runs;
				 * an unmaterialized slot means a reentrant resolution. */
				zend_value_error("deepclone_from_array(): malformed payload, object id " ZEND_LONG_FMT " is not materialized", id);
				return;
			}
		} else {
			/* Guard against ZEND_LONG_MIN — negating it is signed-overflow UB. */
			if (UNEXPECTED(id <= ZEND_LONG_MIN)) {
				zend_value_error("deepclone_from_array(): malformed payload, ref id out of range");
				return;
			}
			target = zend_hash_index_find(refs, -id);
			if (UNEXPECTED(!target)) {
				zend_value_error("deepclone_from_array(): malformed payload, unknown ref id " ZEND_LONG_FMT, -id);
				return;
			}
			/* A hard-ref consumer (mask false) may already have reified this
			 * slot into a zend_reference. The object-ref marker is a by-value
			 * link: deref so the result does not depend on which consumer
			 * resolved first; under lazy hydration that order is the user's
			 * touch order. */
			ZVAL_DEREF(target);
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
				if ((zend_ulong) id >= num_objects
				 || Z_TYPE(objects[id]) != IS_OBJECT) {
					zend_value_error("deepclone_from_array(): malformed payload, named-closure references unknown id " ZEND_LONG_FMT, id);
					return;
				}
				target = &objects[id];
			} else {
				/* Guard against ZEND_LONG_MIN — negating it is signed-overflow UB. */
				if (UNEXPECTED(id <= ZEND_LONG_MIN)) {
					zend_value_error("deepclone_from_array(): malformed payload, named-closure references unknown id " ZEND_LONG_FMT, id);
					return;
				}
				target = zend_hash_index_find(refs, -id);
				if (!target) {
					zend_value_error("deepclone_from_array(): malformed payload, named-closure references unknown id " ZEND_LONG_FMT, id);
					return;
				}
				/* By-value link: see the object-ref marker above. */
				ZVAL_DEREF(target);
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

	if (DC_MASK_IS_CONSTEXPR_CLOSURE(mask)) {
		dc_cexpr_resolve(value, allowed_set, retval);
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
			dc_resolve(slot, mval, objects, num_objects, refs, allowed_set, &resolved);
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

/* ── Lazy hydration via native lazy ghosts ───────────────────
 *
 * On PHP 8.4+, deepclone_from_array() creates the object nodes that are
 * expensive to hydrate as uninitialized lazy ghosts whose property hydration
 * is deferred until the engine first needs them (on older versions
 * everything hydrates eagerly). Every zend_object identity still exists
 * when deepclone_from_array() returns: back-references and shared hard refs
 * bind to the ghosts, and === stays correct under any realization order.
 * Only the per-object property work is deferred: payload zval copies, engine
 * property writes and value-marker resolution (named closures, const-expr
 * closure re-evaluation, enum cases).
 *
 * Eligibility (everything else hydrates eagerly; mixing is the design):
 *   - at least one named-closure or const-expr-closure marker among the
 *     node's property slots or in its deferred __unserialize state mask:
 *     resolving those (fake-closure creation, attribute-args
 *     re-evaluation) is the hydration work worth deferring.
 *     Plain value slots are cheaper to hydrate than to ghost (COW makes
 *     them refcount bumps), so nodes without closure markers gain nothing
 *     and stay eager;
 *   - user classes with no internal ancestor besides stdClass (engine
 *     limitation, mirrors zend_class_can_be_lazy());
 *   - at least one declared property: the engine silently downgrades
 *     zero-declared-prop instances (stdClass included) to non-lazy;
 *   - not a native-serialize ("X:"-prefixed) node.
 * Closure-bearing __wakeup/__unserialize nodes do qualify: their hook runs
 * at the end of their own initialization instead of in the global,
 * children-first phase-9 sequence (the per-entry validation stays eager,
 * only the calls move).
 *
 * All shared state lives in a DeepClone\HydrationContext instance. The
 * ghosts' initializer is a Closure bound to the context (its private
 * hydrate() method, implemented in C below), so
 * ReflectionClass::getLazyInitializer() returns a genuine Closure and every
 * uninitialized ghost keeps the context alive. The
 * context in turn retains the payload (the slot index points into it), the
 * object table (back-reference targets must outlive the call), the shared
 * refs table and a copy of the allow-list. The resulting context↔ghost
 * reference cycle is visible to the GC through dc_lazy_ctx_get_gc() and the
 * engine's lazy-object get_gc, so abandoned graphs are collectable.
 *
 * Structural validation (object ids, scopes, declared-property matches) and
 * the const-expr-closure allow-list gate stay eager: malformed payloads keep
 * failing inside deepclone_from_array(). What can surface lazily, at first
 * access, are value-level resolution errors (unknown class or enum case,
 * stale const-expr line, type errors), like any native lazy-object
 * initializer. A failing initializer is reverted by the engine and the
 * ghost stays uninitialized and retryable. */

typedef struct {
	zend_class_entry   *scope_ce;   /* resolved scope (zend_standard_class_def for public scope) */
	zend_property_info *pi;         /* backed declared property, or NULL → dynamic write */
	zend_string        *name;       /* property name (owned iff name_owned) */
	zval               *value;      /* borrowed pointer into the retained payload */
	zval               *mask;       /* borrowed resolve marker, or NULL */
	bool                name_owned;
} dc_lazy_slot;

/* Deferred __wakeup/__unserialize replay of one ghost: the hook runs at the
 * end of the ghost's initialization instead of in the global phase-9
 * sequence. Phase 9 still validates every entry eagerly; only the calls
 * move. */
typedef struct {
	zval *props;    /* __unserialize argument (borrowed payload pointer), or NULL */
	zval *mask;     /* its resolve mask, or NULL */
	bool  wakeup;   /* call __wakeup() after replaying the slots */
} dc_lazy_state;

typedef struct {
	zval          payload;          /* the $data array, retained: keeps slot pointers alive */
	zval         *objects;          /* strong refs, dense by object id */
	uint32_t      num_objects;
	HashTable     refs;             /* rid => value (shared zend_references once reified) */
	HashTable    *allowed_set;      /* owned copy of the lowercased allow-list, or NULL */
	HashTable     handle_to_id;     /* ghost handle => object id; removed once hydrated */
	dc_lazy_slot *slots;            /* property slots of ghost ids, grouped by id */
	uint32_t     *slot_off;         /* id => slots[slot_off[id] .. slot_off[id+1]) */
	uint32_t      num_slots;        /* filled extent of slots[]; NOT derived from
	                                 * slot_off/num_objects, which the error-path
	                                 * teardown resets while owned slot names still
	                                 * need releasing */
	dc_lazy_state *states;          /* per-id deferred state replays, or NULL */
	zend_object   std;
} dc_lazy_ctx;

/* Set on a handle_to_id value while that id's slots are being replayed, so a
 * re-entrant manual initializer call on the same object (from an autoloader or any
 * user code the hydration runs) cannot hydrate it a second time and delete
 * the handle mid-initialization. Width-relative bit: object ids are memory
 * bound (every id owns a zval slot) far below 2^30 on 32-bit zend_long and
 * below 2^62 on 64-bit. */
#define DC_LAZY_HYDRATING (((zend_long) 1) << (SIZEOF_ZEND_LONG * 8 - 2))

static zend_class_entry *dc_lazy_ctx_ce;
static zend_object_handlers dc_lazy_ctx_handlers;
static zend_function *dc_lazy_hydrate_fn;

static zend_always_inline dc_lazy_ctx *dc_lazy_ctx_from_obj(zend_object *obj)
{
	return (dc_lazy_ctx *)((char *) obj - offsetof(dc_lazy_ctx, std));
}

static zend_object *dc_lazy_ctx_create(zend_class_entry *ce)
{
	dc_lazy_ctx *ctx = zend_object_alloc(sizeof(dc_lazy_ctx), ce);

	ZVAL_UNDEF(&ctx->payload);
	ctx->objects = NULL;
	ctx->num_objects = 0;
	ctx->allowed_set = NULL;
	ctx->slots = NULL;
	ctx->slot_off = NULL;
	ctx->num_slots = 0;
	ctx->states = NULL;
	zend_hash_init(&ctx->refs, 4, NULL, ZVAL_PTR_DTOR, 0);
	zend_hash_init(&ctx->handle_to_id, 8, NULL, NULL, 0);

	zend_object_std_init(&ctx->std, ce);
	object_properties_init(&ctx->std, ce);
	ctx->std.handlers = &dc_lazy_ctx_handlers;

	return &ctx->std;
}

static void dc_lazy_ctx_free(zend_object *object)
{
	dc_lazy_ctx *ctx = dc_lazy_ctx_from_obj(object);

	if (ctx->objects) {
		for (uint32_t i = 0; i < ctx->num_objects; i++) {
			zval_ptr_dtor(&ctx->objects[i]);
		}
		efree(ctx->objects);
	}
	if (ctx->slots) {
		for (uint32_t i = 0; i < ctx->num_slots; i++) {
			if (ctx->slots[i].name_owned) {
				zend_string_release(ctx->slots[i].name);
			}
		}
		efree(ctx->slots);
	}
	if (ctx->slot_off) {
		efree(ctx->slot_off);
	}
	if (ctx->states) {
		efree(ctx->states);
	}
	zend_hash_destroy(&ctx->refs);
	zend_hash_destroy(&ctx->handle_to_id);
	zval_ptr_dtor(&ctx->payload);
	if (ctx->allowed_set) {
		zend_hash_destroy(ctx->allowed_set);
		efree(ctx->allowed_set);
	}
	zend_object_std_dtor(&ctx->std);
}

/* Reported edges: the payload, every object slot (eager instances and
 * ghosts), and, through the returned table, the shared refs. This is what
 * makes the context↔ghost cycle collectable once a graph is abandoned. */
static HashTable *dc_lazy_ctx_get_gc(zend_object *object, zval **table, int *n)
{
	dc_lazy_ctx *ctx = dc_lazy_ctx_from_obj(object);
	zend_get_gc_buffer *gc_buffer = zend_get_gc_buffer_create();

	zend_get_gc_buffer_add_zval(gc_buffer, &ctx->payload);
	for (uint32_t i = 0; i < ctx->num_objects; i++) {
		zend_get_gc_buffer_add_zval(gc_buffer, &ctx->objects[i]);
	}
	zend_get_gc_buffer_use(gc_buffer, table, n);

	return &ctx->refs;
}

#if PHP_VERSION_ID >= 80400
/* Ghost creation and the slot-index build only exist where native lazy
 * objects do; on older versions everything below compiles out and
 * deepclone_from_array() stays fully eager. */

/* Engine eligibility, mirroring zend_object_make_lazy(): internal classes
 * (stdClass excepted) and internal ancestors are rejected; classes without
 * declared properties are silently created non-lazy, so treat them as
 * eager upfront. */
static bool dc_class_can_be_ghost(const zend_class_entry *ce)
{
	if (ce->type != ZEND_USER_CLASS
	 || ce->default_properties_count == 0
	 || (ce->ce_flags & ZEND_ACC_UNINSTANTIABLE)) {
		return false;
	}
	for (const zend_class_entry *parent = ce->parent; parent; parent = parent->parent) {
		if (parent->type == ZEND_INTERNAL_CLASS && parent != zend_standard_class_def) {
			return false;
		}
	}
	return true;
}

/* Eagerly enforce the allow-list on const-expr-closure markers inside a
 * deferred slot, replicating the gate dc_cexpr_resolve() applies before
 * zend_lookup_class(). Without this, lazy mode would delay the "class not
 * allowed" error to an arbitrary later point in the program. Only
 * well-shaped entries are checked: shape errors keep failing at resolve
 * time, exactly like the eager path reports them. */
static void dc_lazy_gate_cexpr(zval *value, zval *mask, HashTable *allowed_set)
{
	if (UNEXPECTED(dc_check_stack_limit())) {
		return;
	}
	if (DC_MASK_IS_CONSTEXPR_CLOSURE(mask)) {
		if (Z_TYPE_P(value) == IS_ARRAY) {
			zval *zclass = zend_hash_index_find(Z_ARRVAL_P(value), 0);
			if (zclass) {
				ZVAL_DEREF(zclass);
				if (Z_TYPE_P(zclass) == IS_STRING
				 && !dc_class_allowed(allowed_set, Z_STR_P(zclass))) {
					zend_value_error("deepclone_from_array(): class \"%s\" is not allowed", Z_STRVAL_P(zclass));
				}
			}
		}
		return;
	}
	if (Z_TYPE_P(mask) != IS_ARRAY || Z_TYPE_P(value) != IS_ARRAY) {
		return;
	}
	zend_string *mkey;
	zend_ulong midx;
	zval *mval;
	ZEND_HASH_FOREACH_KEY_VAL(Z_ARRVAL_P(mask), midx, mkey, mval) {
		zval *slot = mkey
			? zend_hash_find(Z_ARRVAL_P(value), mkey)
			: zend_hash_index_find(Z_ARRVAL_P(value), midx);
		if (!slot) continue;
		dc_lazy_gate_cexpr(slot, mval, allowed_set);
		if (UNEXPECTED(EG(exception))) {
			return;
		}
	} ZEND_HASH_FOREACH_END();
}

/* Build the per-object slot index: one pass over the transposed
 * [scope][name][id] payload columns, mirroring the eager phase-8 walk.
 * Per-column scope and declared-property resolution is hoisted and cached
 * in the slots; per-ghost-id structural validation (scope instanceof,
 * declared-property match) runs here, eagerly, with the same error messages
 * as the eager path; phase 8 still validates everything for eager ids.
 * Slot value/mask pointers point into the payload, which the context
 * retains and nothing ever mutates.
 *
 * The capacity per id was pre-counted (lazy_slot_counts) with the exact
 * skip conditions used here; columns that error out in phase 8 anyway
 * (non-string scope keys, non-array levels) are skipped consistently by
 * both passes. */
static bool dc_lazy_index_build(dc_lazy_ctx *ctx, HashTable *properties_ht, HashTable *resolve_ht,
	zend_string **class_names, zend_class_entry **class_ces, uint32_t num_classes,
	const bool *is_ghost, const uint32_t *lazy_slot_counts)
{
	HashTable *allowed_set = ctx->allowed_set;
	uint32_t num_objects = ctx->num_objects;
	uint32_t total = 0;

	ctx->slot_off = safe_emalloc(num_objects + 1, sizeof(uint32_t), 0);
	for (uint32_t id = 0; id < num_objects; id++) {
		ctx->slot_off[id] = total;
		if (is_ghost[id]) {
			/* Reaching this requires ~2^32 resident payload buckets, but the
			 * accumulation must not wrap into an undersized buffer (the
			 * objectMeta count carries the same kind of sanity cap). */
			if (UNEXPECTED(lazy_slot_counts[id] > UINT32_MAX - total)) {
				zend_value_error("deepclone_from_array(): Argument #1 ($data) \"properties\" slot count out of range");
				return false;
			}
			total += lazy_slot_counts[id];
		}
	}
	ctx->slot_off[num_objects] = total;
	/* Zero-filled so that the free path (and a partially filled index on an
	 * error path) only ever sees NULL names / unowned slots. num_slots is the
	 * allocated extent: the fill below is sparse (per-id cursors), so the
	 * release loop must walk the whole zeroed capacity. */
	ctx->slots = ecalloc(total ? total : 1, sizeof(dc_lazy_slot));
	ctx->num_slots = total;

	if (!properties_ht) {
		/* Ghosts whose only deferred work is a state replay: empty index. */
		return true;
	}

	uint32_t *cursor = safe_emalloc(num_objects ? num_objects : 1, sizeof(uint32_t), 0);
	memcpy(cursor, ctx->slot_off, num_objects * sizeof(uint32_t));

	bool ok = false;
	zend_string *scope_name;
	zval *scope_props;
	ZEND_HASH_FOREACH_STR_KEY_VAL(properties_ht, scope_name, scope_props) {
		if (!scope_name || Z_TYPE_P(scope_props) != IS_ARRAY) {
			continue; /* phase 8 reports these */
		}

		HashTable *resolve_scope = NULL;
		if (resolve_ht) {
			zval *rs = zend_hash_find(resolve_ht, scope_name);
			if (rs && Z_TYPE_P(rs) == IS_ARRAY) {
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
			scope_ce = zend_lookup_class_ex(scope_name, NULL, ZEND_FETCH_CLASS_NO_AUTOLOAD);
		}
		if (UNEXPECTED(!scope_ce)) {
			zend_value_error("deepclone_from_array(): Argument #1 ($data) \"properties\" scope \"%s\" is not a loaded class name",
				ZSTR_VAL(scope_name));
			goto done;
		}
		bool scope_is_std = scope_ce == zend_standard_class_def;

		zend_string *prop_name;
		zend_ulong prop_idx;
		zval *id_values;
		ZEND_HASH_FOREACH_KEY_VAL(Z_ARRVAL_P(scope_props), prop_idx, prop_name, id_values) {
			if (Z_TYPE_P(id_values) != IS_ARRAY) {
				continue; /* phase 8 reports it */
			}
			bool prop_is_numeric = (prop_name == NULL);
			zend_string *name = prop_name;
			if (prop_is_numeric) {
				name = zend_long_to_str((zend_long) prop_idx);
				/* Satisfy the known-hash probes below. */
				zend_string_hash_val(name);
			}

			HashTable *resolve_ids = NULL;
			if (resolve_scope) {
				zval *ri = prop_is_numeric
					? zend_hash_index_find(resolve_scope, prop_idx)
					: zend_hash_find(resolve_scope, prop_name);
				if (ri && Z_TYPE_P(ri) == IS_ARRAY) {
					resolve_ids = Z_ARRVAL_P(ri);
				}
			}

			/* Hoisted declared-property lookup, as in phase 8. */
			zend_property_info *pi = NULL;
			if (!prop_is_numeric && !scope_is_std) {
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
				if (obj_id >= num_objects || !is_ghost[obj_id]) {
					continue; /* eager ids and bounds errors: phase 8 */
				}
				zend_object *obj = Z_OBJ(ctx->objects[obj_id]);

				if (!scope_is_std && !instanceof_function(obj->ce, scope_ce)) {
					zend_value_error("deepclone_from_array(): Argument #1 ($data) \"properties\" scope \"%s\" is not a parent of object id " ZEND_ULONG_FMT " (%s)",
						ZSTR_VAL(scope_name), obj_id, ZSTR_VAL(obj->ce->name));
					goto prop_err;
				}

				zend_property_info *use_pi = pi;
				if (!use_pi && scope_is_std && obj->ce != zend_standard_class_def) {
					zval *zv = zend_hash_find_known_hash(&obj->ce->properties_info, name);
					if (zv) {
						zend_property_info *candidate = Z_PTR_P(zv);
						if (dc_is_std_scope_property(candidate)) {
							use_pi = candidate;
						} else {
							zend_value_error("deepclone_from_array(): Argument #1 ($data) \"properties\" value for \"%s::%s\" targets a non-public declared property on object id " ZEND_ULONG_FMT,
								ZSTR_VAL(scope_name), ZSTR_VAL(name), obj_id);
							goto prop_err;
						}
					}
				} else if (!use_pi && !scope_is_std) {
					zend_value_error("deepclone_from_array(): Argument #1 ($data) \"properties\" value for \"%s::%s\" does not match a declared property on object id " ZEND_ULONG_FMT,
						ZSTR_VAL(scope_name), ZSTR_VAL(name), obj_id);
					goto prop_err;
				}

				zval *marker = resolve_ids ? zend_hash_index_find(resolve_ids, obj_id) : NULL;
				if (marker && allowed_set) {
					dc_lazy_gate_cexpr(prop_val, marker, allowed_set);
					if (UNEXPECTED(EG(exception))) {
						goto prop_err;
					}
				}

				dc_lazy_slot *slot = &ctx->slots[cursor[obj_id]++];
				slot->scope_ce = scope_ce;
				slot->pi = dc_is_backed_declared_property(use_pi) ? use_pi : NULL;
				slot->name = prop_is_numeric ? zend_string_copy(name) : name;
				slot->name_owned = prop_is_numeric;
				slot->value = prop_val;
				slot->mask = marker;
			} ZEND_HASH_FOREACH_END();

			if (prop_is_numeric) {
				zend_string_release(name);
			}
			continue;

prop_err:
			if (prop_is_numeric) {
				zend_string_release(name);
			}
			goto done;
		} ZEND_HASH_FOREACH_END();
	} ZEND_HASH_FOREACH_END();

	ok = true;
done:
	efree(cursor);
	return ok;
}
#endif /* PHP_VERSION_ID >= 80400 */

/* Replay the property slots of one object id, mirroring the eager phase-8
 * write semantics, then run its deferred __wakeup/__unserialize replay
 * (the phase-9 work for this node, so user code may execute here). Runs
 * with the caller's EG(fake_scope) cleared: initialization can be
 * triggered from anywhere, including code that has a fake scope set, and
 * the engine does not reset it around initializer calls. */
static void dc_lazy_hydrate(dc_lazy_ctx *ctx, zend_object *obj, uint32_t id)
{
#if PHP_VERSION_ID >= 80500
	const zend_class_entry *saved_scope = EG(fake_scope);
#else
	zend_class_entry *saved_scope = EG(fake_scope);
#endif
	EG(fake_scope) = NULL;

	uint32_t end = ctx->slot_off[id + 1];
	for (uint32_t i = ctx->slot_off[id]; i < end; i++) {
		dc_lazy_slot *slot = &ctx->slots[i];
		if (UNEXPECTED(!slot->name)) {
			continue; /* unfilled defensive gap, cannot happen on success paths */
		}

		zval final_val;
		if (slot->mask) {
			ZVAL_UNDEF(&final_val);
			dc_resolve(slot->value, slot->mask, ctx->objects, ctx->num_objects,
				&ctx->refs, ctx->allowed_set, &final_val);
			if (UNEXPECTED(EG(exception))) {
				goto restore;
			}
		} else {
			ZVAL_COPY(&final_val, slot->value);
		}

		if (slot->pi) {
			EG(fake_scope) = slot->scope_ce != zend_standard_class_def ? slot->scope_ce : NULL;
			bool ok = dc_write_backed_property(obj, slot->pi, slot->name, &final_val, 0);
			EG(fake_scope) = NULL;
			zval_ptr_dtor(&final_val);
			if (UNEXPECTED(!ok)) {
				goto restore;
			}
		} else {
			/* Dynamic property: same engine route as the eager path. */
			zend_update_property_ex(slot->scope_ce, obj, slot->name, &final_val);
			zval_ptr_dtor(&final_val);
			if (UNEXPECTED(EG(exception))) {
				goto restore;
			}
		}
	}

	/* Deferred state replay: same resolution and calls as phase 9, scoped to
	 * this node, after its slots. The hook may transparently initialize
	 * other ghosts it reaches. */
	if (ctx->states) {
		dc_lazy_state *st = &ctx->states[id];
		if (st->props) {
			zval resolved;
			if (st->mask) {
				ZVAL_UNDEF(&resolved);
				dc_resolve(st->props, st->mask, ctx->objects, ctx->num_objects,
					&ctx->refs, ctx->allowed_set, &resolved);
				if (UNEXPECTED(EG(exception))) {
					goto restore;
				}
			} else {
				ZVAL_COPY(&resolved, st->props);
			}
			/* Flagged-but-method-less classes are rejected by the eager
			 * phase-9 validation; the guard only matters for initializers
			 * triggered before phase 9 ran, where the whole call is about
			 * to fail anyway. */
			if (EXPECTED(obj->ce->__unserialize != NULL)) {
				zend_call_method_with_1_params(obj, obj->ce,
					&obj->ce->__unserialize, "__unserialize", NULL, &resolved);
			}
			zval_ptr_dtor(&resolved);
			if (UNEXPECTED(EG(exception))) {
				goto restore;
			}
		} else if (st->wakeup) {
			zend_function *wakeup_fn = zend_hash_find_ptr(&obj->ce->function_table, ZSTR_KNOWN(ZEND_STR_WAKEUP));
			if (wakeup_fn) {
				zend_call_method_with_0_params(obj, obj->ce, &wakeup_fn, "__wakeup", NULL);
				if (UNEXPECTED(EG(exception))) {
					goto restore;
				}
			}
		}
	}

restore:
	EG(fake_scope) = saved_scope;
}

ZEND_METHOD(DeepClone_HydrationContext, __construct)
{
	ZEND_PARSE_PARAMETERS_NONE();
	/* Private and internal-only; unreachable from userland. */
	zend_throw_error(NULL, "Cannot directly construct DeepClone\\HydrationContext");
}

ZEND_METHOD(DeepClone_HydrationContext, hydrate)
{
	zend_object *target;

	ZEND_PARSE_PARAMETERS_START(1, 1)
		Z_PARAM_OBJ(target)
	ZEND_PARSE_PARAMETERS_END();

	dc_lazy_ctx *ctx = dc_lazy_ctx_from_obj(Z_OBJ_P(ZEND_THIS));
	zval *zid = zend_hash_index_find(&ctx->handle_to_id, target->handle);
	if (UNEXPECTED(!zid || !ctx->slot_off)) {
		zend_value_error("DeepClone\\HydrationContext::hydrate(): Argument #1 ($object) is not an uninitialized lazy ghost of this context");
		RETURN_THROWS();
	}

#if PHP_VERSION_ID >= 80400
	if (zend_object_is_lazy(target)) {
		/* Direct call on a still-lazy ghost (e.g. obtained through
		 * ReflectionClass::getLazyInitializer()): route through the engine
		 * so property snapshots, rollback and flag bookkeeping happen
		 * exactly once; it calls back into this method. */
		zend_lazy_object_init(target);
		return;
	}

	/* The target is not lazy. The only state in which hydrating is correct
	 * is the engine's in-flight initializer invocation: the uninitialized
	 * flag is already cleared but the lazy info is still attached (it is
	 * dropped on success, and a revert re-sets the flag). An object realized
	 * by other means (ReflectionClass::markLazyObjectAsInitialized(),
	 * setRawValueWithoutLazyInitialization() draining the lazy props) has no
	 * info left and must not be hydrated over. */
	if (UNEXPECTED(!zend_hash_index_exists(&EG(lazy_objects_store).infos, target->handle))) {
		zend_value_error("DeepClone\\HydrationContext::hydrate(): Argument #1 ($object) is not an uninitialized lazy ghost of this context");
		RETURN_THROWS();
	}
#endif

	/* Re-entrancy: user code run by the hydration below (autoloaders fired
	 * from marker resolution, __set on dynamic props) could call back with
	 * the same object, which at this point is indistinguishable from the
	 * engine's initializer invocation. A second hydration would double-apply
	 * markers and, worse, delete the handle while the outer attempt can
	 * still fail and be reverted, leaving the ghost permanently
	 * un-hydratable. */
	if (UNEXPECTED(Z_LVAL_P(zid) & DC_LAZY_HYDRATING)) {
		zend_value_error("DeepClone\\HydrationContext::hydrate(): Argument #1 ($object) is already being hydrated");
		RETURN_THROWS();
	}
	uint32_t id = (uint32_t) Z_LVAL_P(zid);
	Z_LVAL_P(zid) |= DC_LAZY_HYDRATING;

	dc_lazy_hydrate(ctx, target, id);

	/* Re-find the entry: nested hydrations of other ghosts may have deleted
	 * entries from the table in the meantime. */
	zid = zend_hash_index_find(&ctx->handle_to_id, target->handle);
	if (UNEXPECTED(EG(exception))) {
		if (EXPECTED(zid != NULL)) {
			Z_LVAL_P(zid) &= ~DC_LAZY_HYDRATING;
		}
		RETURN_THROWS();
	}

	/* Hydrated: the engine drops the initializer (and with it its references
	 * on this context); drop the handle mapping so a stray second call
	 * cannot hydrate the same object twice. */
	zend_hash_index_del(&ctx->handle_to_id, target->handle);
}

/* Throw a ValueError describing malformed input and jump to the cleanup label. */
#define DC_INVALID(...) do { \
		zend_value_error(__VA_ARGS__); \
		goto cleanup; \
	} while (0)
#define DC_REQUIRE(cond, ...) do { if (UNEXPECTED(!(cond))) DC_INVALID(__VA_ARGS__); } while (0)

/* Recursively scan a mask zval tree for LONG(0)/LONG(1) entries (named and
 * const-expr closure markers). Returns true as soon as one is found. */
static bool dc_mask_has_closure(zval *mask)
{
	if (mask == NULL) {
		return false;
	}
	if (DC_MASK_IS_NAMED_CLOSURE(mask) || DC_MASK_IS_CONSTEXPR_CLOSURE(mask)) {
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

/* Like dc_mask_has_closure() but matches only the named-closure marker
 * (LONG(0)), ignoring const-expr-closure references (LONG(1)). */
static bool dc_mask_has_named_closure(zval *mask)
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
		if (dc_mask_has_named_closure(v)) {
			return true;
		}
	} ZEND_HASH_FOREACH_END();
	return false;
}

/* Scan the four payload regions that can carry closure markers — the top
 * mask, the resolve table, the reference masks and the replayed state masks —
 * for a named-closure marker. Mirrors the region set used by the
 * allowed_classes "Closure" gate below. */
static bool dc_payload_has_named_closure(zval *zmask, zval *zresolve, zval *zref_masks, zval *zstates)
{
	if (dc_mask_has_named_closure(zmask)) {
		return true;
	}
	if (zresolve) {
		zval *scope;
		ZEND_HASH_FOREACH_VAL(Z_ARRVAL_P(zresolve), scope) {
			if (Z_TYPE_P(scope) != IS_ARRAY) continue;
			zval *name;
			ZEND_HASH_FOREACH_VAL(Z_ARRVAL_P(scope), name) {
				if (dc_mask_has_named_closure(name)) {
					return true;
				}
			} ZEND_HASH_FOREACH_END();
		} ZEND_HASH_FOREACH_END();
	}
	if (zref_masks) {
		zval *rmask;
		ZEND_HASH_FOREACH_VAL(Z_ARRVAL_P(zref_masks), rmask) {
			if (dc_mask_has_named_closure(rmask)) {
				return true;
			}
		} ZEND_HASH_FOREACH_END();
	}
	if (zstates) {
		zval *state;
		ZEND_HASH_FOREACH_VAL(Z_ARRVAL_P(zstates), state) {
			if (Z_TYPE_P(state) == IS_ARRAY) {
				zval *smask = zend_hash_index_find(Z_ARRVAL_P(state), 2);
				if (smask && dc_mask_has_named_closure(smask)) {
					return true;
				}
			}
		} ZEND_HASH_FOREACH_END();
	}
	return false;
}

PHP_FUNCTION(deepclone_from_array)
{
	HashTable *data_ht;
	HashTable *allowed_ht = NULL;
	HashTable *allowed_set = NULL;
	bool allow_named_closures = false;
	HashTable refs_local;
	HashTable *refs = NULL;
	zend_string **class_names = NULL;
	uint32_t num_classes = 0;
	zend_class_entry **class_ces = NULL;
	zval *objects = NULL;
	uint32_t num_objects = 0;
	uint32_t *obj_class_ids = NULL;
	int *obj_wakeups = NULL;
	bool refs_inited = false;
	/* Lazy hydration: non-NULL once at least one node is ghost-eligible.
	 * The context then owns objects/refs/allowed_set: ghost initializers
	 * must keep seeing them after this call returns. */
	dc_lazy_ctx *lazy_ctx = NULL;
	zval lazy_ctx_zv;
	zval lazy_init_zv;
	bool *is_ghost = NULL;
	uint32_t *lazy_slot_counts = NULL;
	/* Holds the string synthesized for a numeric property name during the
	 * properties walk below, so the shared cleanup label can release it on
	 * any early-exit path. Only one is live at a time. */
	zend_string *numeric_prop_tmp = NULL;

	ZEND_PARSE_PARAMETERS_START(1, 3)
		Z_PARAM_ARRAY_HT(data_ht)
		Z_PARAM_OPTIONAL
		Z_PARAM_ARRAY_HT_OR_NULL(allowed_ht)
		Z_PARAM_BOOL(allow_named_closures)
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

	/* Named closures (the by-name marker, LONG(0)) let a payload mint a
	 * Closure over any function or method by name; they resolve only when the
	 * caller opts in via allow_named_closures, which the producer must also
	 * have set. Const-expr-closure references (LONG(1)) are unaffected: they
	 * resolve only to closures the named class itself declares. The scan runs
	 * before any object is instantiated, so a payload carrying a named closure
	 * is rejected wholesale rather than failing mid-hydration. */
	if (!allow_named_closures
			&& dc_payload_has_named_closure(zmask, zresolve, zref_masks, zstates)) {
		DC_INVALID("deepclone_from_array(): resolving a closure over a named callable requires enabling the \"allow_named_closures\" option; do it only if you trust the input; alternatively, install the \"deepclone\" extension, which can reference callables declared in constant expressions");
	}

	/* ── Build objectMeta ── */
	if (Z_TYPE_P(zobject_meta) == IS_LONG) {
		zend_long n = Z_LVAL_P(zobject_meta);
		if (n < 0) {
			DC_INVALID("deepclone_from_array(): Argument #1 ($data) \"objectMeta\" count must be non-negative, " ZEND_LONG_FMT " given", n);
		}
		/* Sanity cap: the IS_LONG form specifies a count without the per-
		 * object payload, so a tiny input can demand huge allocations
		 * (e.g. objectMeta=2^30 would trigger multi-GB allocs below).
		 * Legitimate use never needs more than ~1M objects in a single
		 * payload — beyond that, use the array form which is naturally
		 * bounded by the hash table size. */
		if (n > (zend_long)(1U << 20)) {
			DC_INVALID("deepclone_from_array(): Argument #1 ($data) \"objectMeta\" count out of range: " ZEND_LONG_FMT, n);
		}
		num_objects = (uint32_t) n;
		if (num_objects > 0) {
			if (num_classes < 1) {
				DC_INVALID("deepclone_from_array(): Argument #1 ($data) \"objectMeta\" references class index 0 but \"classes\" is empty");
			}
			/* IS_LONG form: every object uses class index 0. ecalloc zero-fills. */
			obj_class_ids = ecalloc(num_objects, sizeof(uint32_t));
			/* IS_LONG form implies no state replays — obj_wakeups stays NULL. */
		}
	} else {
		num_objects = zend_hash_num_elements(Z_ARRVAL_P(zobject_meta));
		if (num_objects > 0) {
			obj_class_ids = emalloc(num_objects * sizeof(uint32_t));
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
				/* Lazy-alloc: only pay for the array if at least one entry
				 * actually flags a state replay. Typical payloads (no __wakeup
				 * / __unserialize) keep obj_wakeups NULL and skip the final
				 * validation scan entirely. */
				if (Z_LVAL_P(wk) != 0) {
					if (!obj_wakeups) {
						obj_wakeups = ecalloc(num_objects, sizeof(int));
					}
					obj_wakeups[id] = (int) Z_LVAL_P(wk);
				}
			} else if (Z_TYPE_P(meta) == IS_LONG) {
				cidx_val = Z_LVAL_P(meta);
			} else {
				DC_INVALID("deepclone_from_array(): Argument #1 ($data) \"objectMeta\" entry " ZEND_ULONG_FMT " must be of type int|array, %s given", id, zend_zval_value_name(meta));
			}
			if (cidx_val < 0 || (zend_ulong) cidx_val >= num_classes) {
				DC_INVALID("deepclone_from_array(): Argument #1 ($data) \"objectMeta\" entry " ZEND_ULONG_FMT " has out-of-range class index " ZEND_LONG_FMT, id, cidx_val);
			}
			obj_class_ids[id] = (uint32_t) cidx_val;
		} ZEND_HASH_FOREACH_END();
	}

	/* ── Resolve class entries (once per unique class) ── */
	if (num_classes) {
		class_ces = ecalloc(num_classes, sizeof(zend_class_entry *));
	}

#if PHP_VERSION_ID >= 80400
	/* ── Lazy mode: decide per-node ghost eligibility upfront ──
	 * Only nodes whose hydration includes closure-marker resolution (named
	 * closures, const-expr closures) become ghosts: that work (fake-closure
	 * creation, attribute re-evaluation) is what deferral measurably saves,
	 * while for plain value slots the ghost bookkeeping costs as much as the
	 * hydration it defers and only adds memory and cycle-collector pressure.
	 * The markers live in the [scope][name][id] resolve columns and in the
	 * per-entry "states" masks (for __unserialize state replays), so a
	 * graph without either table, or without closure markers, is hydrated
	 * fully eagerly and no context is created. Class lookups for candidate
	 * nodes happen here instead of in the creation loop below: same lookups,
	 * same ClassNotFoundException, just earlier in the same call. */
	if (num_objects && (zresolve || zstates)) {
		bool *has_closure_slot = ecalloc(num_objects, sizeof(bool));
		bool any_closure = false;
		if (zresolve) {
			zend_string *rs_scope_key;
			zval *rs_scope;
			ZEND_HASH_FOREACH_STR_KEY_VAL(Z_ARRVAL_P(zresolve), rs_scope_key, rs_scope) {
				if (!rs_scope_key || Z_TYPE_P(rs_scope) != IS_ARRAY) continue;
				zval *rs_ids;
				ZEND_HASH_FOREACH_VAL(Z_ARRVAL_P(rs_scope), rs_ids) {
					if (Z_TYPE_P(rs_ids) != IS_ARRAY) continue;
					zend_ulong rs_id;
					zval *rs_mask;
					ZEND_HASH_FOREACH_NUM_KEY_VAL(Z_ARRVAL_P(rs_ids), rs_id, rs_mask) {
						if (rs_id < num_objects && !has_closure_slot[rs_id]
						 && dc_mask_has_closure(rs_mask)) {
							has_closure_slot[rs_id] = true;
							any_closure = true;
						}
					} ZEND_HASH_FOREACH_END();
				} ZEND_HASH_FOREACH_END();
			} ZEND_HASH_FOREACH_END();
		}
		if (zstates) {
			zval *st_entry;
			ZEND_HASH_FOREACH_VAL(Z_ARRVAL_P(zstates), st_entry) {
				if (Z_TYPE_P(st_entry) != IS_ARRAY) continue;
				zval *st_id = zend_hash_index_find(Z_ARRVAL_P(st_entry), 0);
				zval *st_mask = zend_hash_index_find(Z_ARRVAL_P(st_entry), 2);
				if (st_id && Z_TYPE_P(st_id) == IS_LONG && st_mask
				 && Z_LVAL_P(st_id) >= 0 && (zend_ulong) Z_LVAL_P(st_id) < num_objects
				 && !has_closure_slot[Z_LVAL_P(st_id)]
				 && dc_mask_has_closure(st_mask)) {
					has_closure_slot[Z_LVAL_P(st_id)] = true;
					any_closure = true;
				}
			} ZEND_HASH_FOREACH_END();
		}

		bool any_ghost = false;
		if (any_closure) {
			/* Count payload property slots, but only for closure-bearing
			 * ids: they are the only ones the slot index will hold. The
			 * skip conditions must stay a superset of the index-build fill
			 * conditions so capacity always covers the fill. */
			lazy_slot_counts = ecalloc(num_objects, sizeof(uint32_t));
			if (zproperties) {
				zend_string *cnt_scope_key;
				zval *cnt_scope;
				ZEND_HASH_FOREACH_STR_KEY_VAL(Z_ARRVAL_P(zproperties), cnt_scope_key, cnt_scope) {
					if (!cnt_scope_key || Z_TYPE_P(cnt_scope) != IS_ARRAY) continue;
					zval *cnt_ids;
					ZEND_HASH_FOREACH_VAL(Z_ARRVAL_P(cnt_scope), cnt_ids) {
						if (Z_TYPE_P(cnt_ids) != IS_ARRAY) continue;
						zend_ulong cnt_id;
						zval *cnt_unused;
						ZEND_HASH_FOREACH_NUM_KEY_VAL(Z_ARRVAL_P(cnt_ids), cnt_id, cnt_unused) {
							(void) cnt_unused;
							if (cnt_id < num_objects && has_closure_slot[cnt_id]) {
								lazy_slot_counts[cnt_id]++;
							}
						} ZEND_HASH_FOREACH_END();
					} ZEND_HASH_FOREACH_END();
				} ZEND_HASH_FOREACH_END();
			}

			is_ghost = ecalloc(num_objects, sizeof(bool));
			for (uint32_t id = 0; id < num_objects; id++) {
				uint32_t cid = obj_class_ids[id];
				zend_string *class_name = class_names[cid];
				/* A node qualifies when it has something to defer: property
				 * slots, or a __wakeup/__unserialize replay (the hook then
				 * runs at the end of its own initialization instead of in
				 * the global phase-9 sequence). */
				if (!has_closure_slot[id]
				 || !(lazy_slot_counts[id] || (obj_wakeups && obj_wakeups[id] != 0))
				 || (ZSTR_LEN(class_name) > 1 && ZSTR_VAL(class_name)[1] == ':')) {
					continue;
				}
				zend_class_entry *ce = class_ces[cid];
				if (!ce) {
					ce = zend_lookup_class(class_name);
					if (!ce) {
						efree(has_closure_slot);
						zend_throw_exception_ex(dc_ce_class_not_found_exception, 0,
							"Class \"%s\" not found.", ZSTR_VAL(class_name));
						goto cleanup;
					}
					class_ces[cid] = ce;
				}
				if (dc_class_can_be_ghost(ce)) {
					is_ghost[id] = true;
					any_ghost = true;
				}
			}
		}
		efree(has_closure_slot);

		if (any_ghost) {
			ZVAL_OBJ(&lazy_ctx_zv, dc_lazy_ctx_create(dc_lazy_ctx_ce));
			lazy_ctx = dc_lazy_ctx_from_obj(Z_OBJ(lazy_ctx_zv));

			/* Retain the payload: the slot index points into it, and nothing
			 * mutates it (userland writes COW-separate their own copy). */
			GC_TRY_ADDREF(data_ht);
			ZVAL_ARR(&lazy_ctx->payload, data_ht);
			if (UNEXPECTED(GC_FLAGS(data_ht) & GC_IMMUTABLE)) {
				Z_TYPE_INFO(lazy_ctx->payload) = IS_ARRAY; /* not refcounted */
			}

			/* The context owns the allow-list copy from now on: deferred
			 * dc_resolve() calls must keep seeing it: a NULL set would mean
			 * allow-all, i.e. a lazy-only filter bypass. */
			lazy_ctx->allowed_set = allowed_set;

			/* The initializer stored on every ghost: a Closure over the
			 * context's hydrate() method, so userland introspection
			 * (ReflectionClass::getLazyInitializer()) sees a plain Closure.
			 * The engine-side fcc below targets the method directly; the
			 * Closure is what zend_object_make_lazy() retains as the zv. */
			zend_create_fake_closure(&lazy_init_zv, dc_lazy_hydrate_fn,
				dc_lazy_ctx_ce, dc_lazy_ctx_ce, &lazy_ctx_zv);
		} else if (is_ghost) {
			efree(is_ghost);
			is_ghost = NULL;
		}
	}
#endif

	/* ── Initialize refs early so cleanup can always destroy it safely ──
	 * In lazy mode the table lives in the context: ghost initializers keep
	 * resolving against the same shared zend_references long after this
	 * call returned. */
	if (lazy_ctx) {
		refs = &lazy_ctx->refs;
	} else {
		zend_hash_init(&refs_local, 4, NULL, ZVAL_PTR_DTOR, 0);
		refs = &refs_local;
		refs_inited = true;
	}

	/* ── Create object instances ───────────────── */
	if (num_objects) {
		objects = emalloc(num_objects * sizeof(zval));
		for (uint32_t i = 0; i < num_objects; i++) {
			ZVAL_UNDEF(&objects[i]);
		}
		if (lazy_ctx) {
			lazy_ctx->objects = objects;
			lazy_ctx->num_objects = num_objects;
		}
	}

	for (uint32_t id = 0; id < num_objects; id++) {
		uint32_t cid = obj_class_ids[id];
		zend_string *class_name = class_names[cid];
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
			/* The result is later dereferenced as a zend_object*; a malformed
			 * payload can carry any serialize form (i:…, s:…, a:…), so reject
			 * anything that did not decode to an object before storing it. */
			if (UNEXPECTED(Z_TYPE(obj_zval) != IS_OBJECT)) {
				const char *got = zend_zval_value_name(&obj_zval);
				zval_ptr_dtor(&obj_zval);
				DC_INVALID("deepclone_from_array(): Argument #1 ($data) object %u did not unserialize to an object, %s given", id, got);
			}
		} else {
			/* class_ces is lazily populated — fill on miss. */
			zend_class_entry *ce = class_ces[cid];
			if (!ce) {
				ce = zend_lookup_class(class_name);
				if (!ce) {
					zend_throw_exception_ex(dc_ce_class_not_found_exception, 0,
						"Class \"%s\" not found.", ZSTR_VAL(class_name));
					goto cleanup;
				}
				class_ces[cid] = ce;
			}
			/* deepclone_to_array() always emits a class that has
			 * __unserialize() as a negative-wakeup state replay. A payload
			 * that creates such a class without that flag would leave the bare
			 * object_init_ex() shell uninitialized — e.g. BcMath\Number's
			 * bc_num stays NULL and any operation on it crashes. Reject rather
			 * than build an unusable object. */
			if (UNEXPECTED(ce->__unserialize != NULL && (!obj_wakeups || obj_wakeups[id] >= 0))) {
				DC_INVALID("deepclone_from_array(): Argument #1 ($data) object %u of class %s has an __unserialize() method but \"objectMeta\" does not flag it for an __unserialize state", id, ZSTR_VAL(ce->name));
			}
#if PHP_VERSION_ID >= 80400
			if (is_ghost && is_ghost[id]) {
				/* Lazy node: create an uninitialized ghost whose initializer
				 * is the Closure over the shared context's hydrate() method.
				 * zend_object_make_lazy() duplicates the fcc and the zv, so
				 * every ghost holds strong references on the context (via
				 * the fcc directly, and through the Closure's bound $this)
				 * until it is initialized (or dies). */
				zend_fcall_info_cache fcc = empty_fcall_info_cache;
				fcc.function_handler = dc_lazy_hydrate_fn;
				fcc.object = Z_OBJ(lazy_ctx_zv);
				fcc.calling_scope = dc_lazy_ctx_ce;
				fcc.called_scope = dc_lazy_ctx_ce;
				zend_object *ghost = zend_object_make_lazy(NULL, ce,
					&lazy_init_zv, &fcc, ZEND_LAZY_OBJECT_STRATEGY_GHOST);
				if (UNEXPECTED(!ghost)) {
					goto cleanup;
				}
				ZVAL_OBJ(&obj_zval, ghost);
				if (EXPECTED(zend_object_is_lazy(ghost))) {
					zval zid;
					ZVAL_LONG(&zid, id);
					zend_hash_index_add_new(&lazy_ctx->handle_to_id, ghost->handle, &zid);
				} else {
					/* Engine downgrade (no lazy-able property slots after
					 * all): the instance is complete; hydrate it eagerly. */
					is_ghost[id] = false;
				}
			} else
#endif
			if (UNEXPECTED(object_init_ex(&obj_zval, ce) != SUCCESS)) {
				goto cleanup;
			}
		}

		ZVAL_COPY_VALUE(&objects[id], &obj_zval);
	}

#if PHP_VERSION_ID >= 80400
	/* ── Lazy mode: build the per-object slot index ──
	 * Must be complete before anything can trigger a ghost initializer; the
	 * first such opportunity is user code run from phase 8 writes or phase 9
	 * state replays, both after this point. */
	if (lazy_ctx) {
		if (!dc_lazy_index_build(lazy_ctx,
				zproperties ? Z_ARRVAL_P(zproperties) : NULL,
				zresolve ? Z_ARRVAL_P(zresolve) : NULL,
				class_names, class_ces, num_classes,
				is_ghost, lazy_slot_counts)) {
			goto cleanup;
		}

		/* ── Record deferred __wakeup/__unserialize replays ──
		 * Recorded before any user code can trigger an initializer (the
		 * first opportunity is a phase-8 write), so that a ghost touched
		 * from an eager node's hook mid-call still replays its own state
		 * even when its "states" entry comes later in the sequence.
		 * Phase 9 keeps validating every entry eagerly; only the calls are
		 * skipped for ghosts. Recording mirrors what the eager path would
		 * actually call: only from existing entries, first one wins, so a
		 * mid-call touch on a malformed payload (missing or duplicate
		 * entry) cannot run a hook the eager path would not have run
		 * before failing. */
		if (obj_wakeups && zstates) {
			bool any_deferred = false;
			for (uint32_t id = 0; id < num_objects; id++) {
				if (is_ghost[id] && obj_wakeups[id] != 0) {
					any_deferred = true;
					break;
				}
			}
			if (any_deferred) {
				lazy_ctx->states = ecalloc(num_objects, sizeof(dc_lazy_state));
				zval *st_entry;
				ZEND_HASH_FOREACH_VAL(Z_ARRVAL_P(zstates), st_entry) {
					if (Z_TYPE_P(st_entry) == IS_LONG) {
						zend_long wid = Z_LVAL_P(st_entry);
						if (wid >= 0 && (zend_ulong) wid < num_objects
						 && is_ghost[wid] && obj_wakeups[wid] > 0) {
							lazy_ctx->states[wid].wakeup = true;
						}
						continue;
					}
					if (Z_TYPE_P(st_entry) != IS_ARRAY) continue;
					zval *st_id = zend_hash_index_find(Z_ARRVAL_P(st_entry), 0);
					zval *st_props = zend_hash_index_find(Z_ARRVAL_P(st_entry), 1);
					zval *st_mask = zend_hash_index_find(Z_ARRVAL_P(st_entry), 2);
					if (!st_id || Z_TYPE_P(st_id) != IS_LONG || !st_props
					 || Z_LVAL_P(st_id) < 0 || (zend_ulong) Z_LVAL_P(st_id) >= num_objects) {
						continue; /* phase 9 reports malformed entries */
					}
					uint32_t sid = (uint32_t) Z_LVAL_P(st_id);
					if (!is_ghost[sid] || obj_wakeups[sid] >= 0
					 || lazy_ctx->states[sid].props != NULL) {
						continue;
					}
					if (st_mask && allowed_set) {
						/* Same eager const-expr gate as deferred slots. */
						dc_lazy_gate_cexpr(st_props, st_mask, allowed_set);
						if (UNEXPECTED(EG(exception))) {
							goto cleanup;
						}
					}
					lazy_ctx->states[sid].props = st_props;
					lazy_ctx->states[sid].mask = st_mask;
				} ZEND_HASH_FOREACH_END();
			}
		}
	}
#endif

	/* ── Resolve refs ──────────────────────────── */
	if (zrefs && Z_TYPE_P(zrefs) == IS_ARRAY) {
		/* First pass: populate refs with unresolved copies (needed for self-refs) */
		zend_ulong rid;
		zval *rval;
		ZEND_HASH_FOREACH_NUM_KEY_VAL(Z_ARRVAL_P(zrefs), rid, rval) {
			zval copy;
			ZVAL_COPY(&copy, rval);
			zend_hash_index_add_new(refs, rid, &copy);
		} ZEND_HASH_FOREACH_END();

		/* Second pass: resolve those with masks, updating in-place */
		if (zref_masks) {
			zval *rmask;
			ZEND_HASH_FOREACH_NUM_KEY_VAL(Z_ARRVAL_P(zref_masks), rid, rmask) {
				zval *slot = zend_hash_index_find(refs, rid);
				if (!slot) continue;
				zval resolved;
				ZVAL_UNDEF(&resolved);
				dc_resolve(slot, rmask, objects, num_objects, refs, allowed_set, &resolved);
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
			zend_ulong prop_idx;
			zval *id_values;
			ZEND_HASH_FOREACH_KEY_VAL(Z_ARRVAL_P(scope_props), prop_idx, prop_name, id_values) {
				/* A numeric property name (e.g. $o->{'999'}) arrives as an
				 * integer key because PHP normalizes numeric string keys.
				 * Synthesize its string form so the write paths stay uniform;
				 * such names are always dynamic properties, so the declared-
				 * property lookup is skipped and resolve markers (transposed
				 * with the same key) are looked up by the integer index. The
				 * shared cleanup label releases numeric_prop_tmp on any exit. */
				bool prop_is_numeric = (prop_name == NULL);
				if (prop_is_numeric) {
					prop_name = numeric_prop_tmp = zend_long_to_str((zend_long) prop_idx);
					/* Force the hash so the zend_hash_find_known_hash() probes
					 * below (e.g. the stdClass-scoped declared-property lookup on
					 * a typed object) satisfy their "hash must be known" contract. */
					zend_string_hash_val(prop_name);
				}
				if (Z_TYPE_P(id_values) != IS_ARRAY) {
					EG(fake_scope) = old_scope;
					DC_INVALID("deepclone_from_array(): Argument #1 ($data) \"properties\" value for \"%s::%s\" must be of type array, %s given", ZSTR_VAL(scope_name), ZSTR_VAL(prop_name), zend_zval_value_name(id_values));
				}

				/* Get resolve markers for this property */
				HashTable *resolve_ids = NULL;
				if (resolve_scope) {
					zval *ri = prop_is_numeric
						? zend_hash_index_find(resolve_scope, prop_idx)
						: zend_hash_find(resolve_scope, prop_name);
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
				 * in &scope_ce->properties_info so _known_hash is safe. A
				 * numeric name can never be a declared property, so skip it. */
				zend_property_info *pi = NULL;
				if (!prop_is_numeric && scope_ce && scope_ce != zend_standard_class_def) {
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
					if (UNEXPECTED(obj_id >= num_objects)) {
						EG(fake_scope) = old_scope;
						DC_INVALID("deepclone_from_array(): Argument #1 ($data) \"properties\" entry for \"%s::%s\" references unknown object id " ZEND_ULONG_FMT,
							ZSTR_VAL(scope_name), ZSTR_VAL(prop_name), obj_id);
					}
					if (is_ghost && is_ghost[obj_id]) {
						/* Created as a lazy ghost: its slots are replayed by
						 * the initializer. Skip by creation-time flag, never
						 * by current lazy state; user code triggered from an
						 * earlier write may already have initialized it, and
						 * hydrating it a second time here would double-apply
						 * markers. */
						continue;
					}
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
						dc_resolve(prop_val, marker, objects, num_objects, refs, allowed_set, &final_val);
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
						/* Dynamic property on a non-stdClass object. Routed
						 * through zend_update_property_ex() so any overridden
						 * write_property handler (internal classes, extensions)
						 * is respected. Matches the deepclone_hydrate() path. */
						zend_update_property_ex(scope_ce, obj, prop_name, &final_val);
						zval_ptr_dtor(&final_val);
						if (EG(exception)) {
							EG(fake_scope) = old_scope;
							goto cleanup;
						}
					}
				} ZEND_HASH_FOREACH_END();

				if (numeric_prop_tmp) {
					zend_string_release(numeric_prop_tmp);
					numeric_prop_tmp = NULL;
				}
			} ZEND_HASH_FOREACH_END();

			EG(fake_scope) = old_scope;
		} ZEND_HASH_FOREACH_END();
	}

	/* ── States: __unserialize / __wakeup ──────────
	 *
	 * Each entry must match its objectMeta wakeup sign (positive → __wakeup,
	 * negative → __unserialize). As entries are consumed, the matching slot
	 * in obj_wakeups is zeroed; a second reference to the same id will see
	 * a zero slot and be rejected. After the loop, any non-zero wakeup left
	 * in obj_wakeups means the payload advertised a state replay that never
	 * came — also a hard error. */
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
				if (!obj_wakeups || obj_wakeups[Z_LVAL_P(zid)] >= 0) {
					DC_INVALID("deepclone_from_array(): Argument #1 ($data) \"states\" has an __unserialize entry for object id " ZEND_LONG_FMT " but \"objectMeta\" does not flag it for __unserialize", Z_LVAL_P(zid));
				}
				obj_wakeups[Z_LVAL_P(zid)] = 0;
				zval *obj_zval = &objects[Z_LVAL_P(zid)];
				zend_class_entry *unser_ce = Z_OBJCE_P(obj_zval);
				if (!unser_ce->__unserialize) {
					DC_INVALID("deepclone_from_array(): Argument #1 ($data) \"states\" entry references object id " ZEND_LONG_FMT " whose class %s has no __unserialize() method", Z_LVAL_P(zid), ZSTR_VAL(unser_ce->name));
				}
				if (is_ghost && is_ghost[Z_LVAL_P(zid)]) {
					/* Deferred into the ghost's initializer. */
					continue;
				}
				zval resolved_props;
				if (smask) {
					ZVAL_UNDEF(&resolved_props);
					dc_resolve(sprops, smask, objects, num_objects, refs, allowed_set, &resolved_props);
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
				if (!obj_wakeups || obj_wakeups[Z_LVAL_P(state)] <= 0) {
					DC_INVALID("deepclone_from_array(): Argument #1 ($data) \"states\" has a __wakeup entry for object id " ZEND_LONG_FMT " but \"objectMeta\" does not flag it for __wakeup", Z_LVAL_P(state));
				}
				obj_wakeups[Z_LVAL_P(state)] = 0;
				if (is_ghost && is_ghost[Z_LVAL_P(state)]) {
					/* Deferred into the ghost's initializer. */
					continue;
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

	/* Any wakeup slot still non-zero means objectMeta advertised a state
	 * replay that never appeared in "states". */
	if (obj_wakeups) {
		for (uint32_t i = 0; i < num_objects; i++) {
			if (obj_wakeups[i] != 0) {
				DC_INVALID("deepclone_from_array(): Argument #1 ($data) \"objectMeta\" entry %u flags object for state replay but no matching \"states\" entry was found", i);
			}
		}
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
			/* Guard against ZEND_LONG_MIN — negating it is signed-overflow UB. */
			if (UNEXPECTED(id <= ZEND_LONG_MIN)) {
				DC_INVALID("deepclone_from_array(): Argument #1 ($data) \"prepared\" references unknown ref id out of range");
			}
			zval *ref = zend_hash_index_find(refs, -id);
			if (!ref) {
				DC_INVALID("deepclone_from_array(): Argument #1 ($data) \"prepared\" references unknown ref id " ZEND_LONG_FMT, -id);
			}
			/* By-value link, like the object-ref marker in dc_resolve():
			 * deref so a slot already reified by a hard-ref consumer yields
			 * the same value as an untouched one. */
			ZVAL_DEREF(ref);
			ZVAL_COPY(return_value, ref);
		}
	} else if (zmask) {
		dc_resolve(zprepared, zmask, objects, num_objects, refs, allowed_set, return_value);
		if (EG(exception)) goto cleanup;
	} else {
		ZVAL_COPY(return_value, zprepared);
	}

cleanup:
	if (numeric_prop_tmp) zend_string_release(numeric_prop_tmp);
	if (lazy_ctx) {
		/* The context owns objects/refs/allowed_set; every uninitialized
		 * ghost keeps it alive, and the last one to go releases the whole
		 * graph (or the GC collects the context↔ghost cycle). */
		if (UNEXPECTED(EG(exception))) {
			/* Failed mid-hydration: break the cycle deterministically so the
			 * partial graph is freed now, not at the next GC run. Releasing
			 * uninitialized ghosts runs no destructors and drops their
			 * initializer references on the context. */
			zend_hash_clean(&lazy_ctx->refs);
			zval *ctx_objects = lazy_ctx->objects;
			uint32_t ctx_num = lazy_ctx->num_objects;
			lazy_ctx->objects = NULL;
			lazy_ctx->num_objects = 0;
			objects = NULL;
			if (ctx_objects) {
				for (uint32_t i = 0; i < ctx_num; i++) {
					zval_ptr_dtor(&ctx_objects[i]);
				}
				efree(ctx_objects);
			}
		}
		zval_ptr_dtor(&lazy_init_zv);
		zval_ptr_dtor(&lazy_ctx_zv);
	} else {
		if (allowed_set) { zend_hash_destroy(allowed_set); efree(allowed_set); }
		if (refs_inited) zend_hash_destroy(refs);
		if (objects) {
			for (uint32_t i = 0; i < num_objects; i++) {
				zval_ptr_dtor(&objects[i]);
			}
			efree(objects);
		}
	}
	if (is_ghost) efree(is_ghost);
	if (lazy_slot_counts) efree(lazy_slot_counts);
	if (obj_class_ids) efree(obj_class_ids);
	if (obj_wakeups) efree(obj_wakeups);
	if (class_ces)   efree(class_ces);
	if (class_names) efree(class_names);
}
#undef DC_INVALID

/* ── deepclone_hydrate() — instantiate/hydrate from a flat mangled-key array ── */

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
				/* Internal final classes with create_object: the engine refuses
				 * newInstanceWithoutConstructor() for them (INTERNAL + create_object
				 * + FINAL). Hydrate injects properties into a create_object shell,
				 * but a class whose validity depends on __construct()/__unserialize()
				 * (e.g. BcMath\Number, whose bc_num stays NULL until then) cannot be
				 * built that way. Mirror the polyfill: probe an empty serialization
				 * payload and refuse hydrate if it does not reconstruct an object.
				 * Round-trip (deepclone_from_array) is unaffected — it replays the
				 * real state through __unserialize(). */
				if (ok && has_ser_api && ce->create_object != NULL
				 && (ce->ce_flags & ZEND_ACC_FINAL) && ce != php_ce_incomplete_class) {
					smart_str payload = {0};
					smart_str_appendc(&payload, (ce->serialize != NULL && !has_unser) ? 'C' : 'O');
					smart_str_appendc(&payload, ':');
					smart_str_append_long(&payload, (zend_long) ZSTR_LEN(ce->name));
					smart_str_appendl(&payload, ":\"", 2);
					smart_str_append(&payload, ce->name);
					smart_str_appendl(&payload, "\":0:{}", 6);
					smart_str_0(&payload);

					zval probe;
					ZVAL_UNDEF(&probe);
					const unsigned char *p = (const unsigned char *) ZSTR_VAL(payload.s);
					const unsigned char *pend = p + ZSTR_LEN(payload.s);
					/* Raise serialize_lock so the probe gets an isolated
					 * php_unserialize_data: otherwise, when hydrate runs inside
					 * an active unserialize (e.g. Serializable::unserialize()),
					 * PHP_VAR_UNSERIALIZE_INIT would reuse the outer context's
					 * data and DESTROY would skip var_destroy (level>1), so the
					 * deferred __unserialize([])/__wakeup() never fires here and
					 * the probe object leaks into the outer dtor list. */
					BG(serialize_lock)++;
					php_unserialize_data_t var_hash;
					PHP_VAR_UNSERIALIZE_INIT(var_hash);
					bool shell_ok = php_var_unserialize(&probe, &p, pend, &var_hash)
						&& Z_TYPE(probe) == IS_OBJECT;
					PHP_VAR_UNSERIALIZE_DESTROY(var_hash);
					BG(serialize_lock)--;
					if (EG(exception)) {
						zend_clear_exception();
						shell_ok = false;
					}
					zval_ptr_dtor(&probe);
					smart_str_free(&payload);
					if (!shell_ok) {
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

	if (!vars || !zend_hash_num_elements(vars)) {
		ZVAL_COPY_VALUE(return_value, &obj_zval);
		return;
	}

	zend_object *obj = Z_OBJ(obj_zval);
	zend_class_entry *obj_ce = obj->ce;

	/* One-time rebuild for stdClass — avoids the check inside the write loop. */
	if (obj_ce == zend_standard_class_def && UNEXPECTED(!obj->properties)) {
		rebuild_object_properties_internal(obj);
	}

	/* $vars keys are interpreted in the same shape `(array) $obj` produces:
	 *   "propName"              → public, or private declared on obj_ce
	 *   "\0*\0propName"         → protected (declaring class resolved via obj_ce)
	 *   "\0ClassName\0propName" → private declared on ClassName (must be obj_ce
	 *                             or a parent; interfaces are not valid scopes)
	 */
	zend_ulong prop_idx;
	zend_string *prop_key;
	zval *prop_val;
	ZEND_HASH_FOREACH_KEY_VAL(vars, prop_idx, prop_key, prop_val) {
		/* Integer keys: coerce to string on dynamic property access, matching
		 * unserialize()'s permissiveness. Allocated name is released below. */
		bool prop_key_owned = false;
		if (!prop_key) {
			prop_key = zend_long_to_str((zend_long) prop_idx);
			prop_key_owned = true;
		}

		const char *key = ZSTR_VAL(prop_key);
		size_t key_len = ZSTR_LEN(prop_key);

		/* Resolve (scope_ce, real_name) from the mangled key shape.
		 * is_mangled is set when the key started with NUL — if the resolved
		 * property_info lookup then misses, we reject the key rather than
		 * silently creating a dynamic property, since a mangled-form key
		 * specifically targets a declared protected / private slot. */
		zend_class_entry *scope_ce = obj_ce;
		zend_string *real_name = prop_key;
		bool real_name_owned = false;
		bool is_mangled = false;

		if (key_len > 0 && key[0] == '\0') {
			is_mangled = true;
			/* Find second NUL separator */
			const char *sep = memchr(key + 1, '\0', key_len - 1);
			if (UNEXPECTED(!sep || sep == key + 1)) {
				zend_value_error("deepclone_hydrate(): Argument #2 ($vars) contains an invalid mangled key");
				if (prop_key_owned) zend_string_release(prop_key);
				zval_ptr_dtor(&obj_zval);
				RETURN_THROWS();
			}
			size_t class_len = sep - (key + 1);
			size_t name_len = key_len - class_len - 2;

			/* Reject embedded NUL in the property name portion */
			if (UNEXPECTED(memchr(sep + 1, '\0', name_len) != NULL)) {
				zend_value_error("deepclone_hydrate(): Argument #2 ($vars) contains an invalid mangled key");
				if (prop_key_owned) zend_string_release(prop_key);
				zval_ptr_dtor(&obj_zval);
				RETURN_THROWS();
			}
			real_name = zend_string_init(sep + 1, name_len, 0);
			real_name_owned = true;

			if (class_len != 1 || key[1] != '*') {
				/* "\0ClassName\0propName" — resolve and validate the scope.
				 * Must be obj_ce or a parent; no autoloader is triggered. */
				if (class_len == ZSTR_LEN(obj_ce->name)
				    && !memcmp(key + 1, ZSTR_VAL(obj_ce->name), class_len)) {
					scope_ce = obj_ce;
				} else {
					scope_ce = NULL;
					for (zend_class_entry *p = obj_ce->parent; p; p = p->parent) {
						if (class_len == ZSTR_LEN(p->name)
						    && !memcmp(key + 1, ZSTR_VAL(p->name), class_len)) {
							scope_ce = p;
							break;
						}
					}
					if (UNEXPECTED(!scope_ce)) {
						zend_value_error("deepclone_hydrate(): Argument #2 ($vars) key scope \"%.*s\" is not a parent of \"%s\"",
							(int) class_len, key + 1, ZSTR_VAL(obj_ce->name));
						zend_string_release(real_name);
						if (prop_key_owned) zend_string_release(prop_key);
						zval_ptr_dtor(&obj_zval);
						RETURN_THROWS();
					}
				}
			}
			/* "\0*\0propName" keeps scope_ce = obj_ce and lets properties_info
			 * lookup find the entry (protected props are inherited under bare
			 * name, so pi->ce will point at the declaring class). */
		}

		/* Default: the input's PHP & references are dropped (dereferenced) on
		 * write. Pass DEEPCLONE_HYDRATE_PRESERVE_REFS to keep the ref link. */
		zval *v = prop_val;
		if (!(flags & DEEPCLONE_HYDRATE_PRESERVE_REFS)) {
			ZVAL_DEREF(v);
		}

		if (obj_ce == zend_standard_class_def) {
			/* Matches unserialize(): NUL-in-middle names are stored as-is on the
			 * stdClass dynamic properties table; NUL-prefix names are rejected
			 * by the engine on read. */
			Z_TRY_ADDREF_P(v);
			zend_hash_update(obj->properties, real_name, v);
		} else {
			zend_property_info *pi = NULL;
			zval *zv = zend_hash_find(&scope_ce->properties_info, real_name);
			if (zv) pi = Z_PTR_P(zv);

			if (dc_is_backed_declared_property(pi)) {
				if (UNEXPECTED(!dc_write_backed_property(obj, pi, real_name, v, flags))) {
					if (real_name_owned) zend_string_release(real_name);
					if (prop_key_owned) zend_string_release(prop_key);
					zval_ptr_dtor(&obj_zval);
					RETURN_THROWS();
				}
			} else if (UNEXPECTED(is_mangled)) {
				/* Mangled-form key but the targeted slot isn't declared.
				 * Reject instead of silently creating a dynamic property —
				 * the caller explicitly asked for a declared protected /
				 * private slot via the "\0*\0name" or "\0Class\0name" form. */
				zend_value_error("deepclone_hydrate(): Argument #2 ($vars) key scope \"%s\" does not declare a \"%s\" property",
					ZSTR_VAL(scope_ce->name), ZSTR_VAL(real_name));
				if (real_name_owned) zend_string_release(real_name);
				if (prop_key_owned) zend_string_release(prop_key);
				zval_ptr_dtor(&obj_zval);
				RETURN_THROWS();
			} else {
				/* Dynamic property or unknown name. Goes through
				 * zend_update_property_ex() so any overridden write_property
				 * handler (internal classes, extensions overriding default
				 * handlers) is respected. */
				zend_update_property_ex(scope_ce, obj, real_name, v);
				if (UNEXPECTED(EG(exception))) {
					if (real_name_owned) zend_string_release(real_name);
					if (prop_key_owned) zend_string_release(prop_key);
					zval_ptr_dtor(&obj_zval);
					RETURN_THROWS();
				}
			}
		}

		if (real_name_owned) zend_string_release(real_name);
		if (prop_key_owned) zend_string_release(prop_key);
	} ZEND_HASH_FOREACH_END();

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

	/* DeepClone\HydrationContext: internal-only object behind lazy ghosts.
	 * Instances are created through dc_lazy_ctx_create() exclusively: the
	 * private constructor blocks `new`, and being an internal FINAL class
	 * with a custom create_object makes the engine refuse
	 * ReflectionClass::newInstanceWithoutConstructor(). */
	dc_lazy_ctx_ce = register_class_DeepClone_HydrationContext();
	dc_lazy_ctx_ce->create_object = dc_lazy_ctx_create;
	memcpy(&dc_lazy_ctx_handlers, &std_object_handlers, sizeof(zend_object_handlers));
	dc_lazy_ctx_handlers.offset = offsetof(dc_lazy_ctx, std);
	dc_lazy_ctx_handlers.free_obj = dc_lazy_ctx_free;
	dc_lazy_ctx_handlers.get_gc = dc_lazy_ctx_get_gc;
	dc_lazy_ctx_handlers.clone_obj = NULL;
	dc_lazy_hydrate_fn = zend_hash_str_find_ptr(&dc_lazy_ctx_ce->function_table,
		"hydrate", sizeof("hydrate") - 1);
	ZEND_ASSERT(dc_lazy_hydrate_fn != NULL);

	register_deepclone_symbols(module_number);

#if PHP_VERSION_ID >= 80500
	/* Cross-class first-class-callable provenance: on PHP 8.5 the engine records
	 * no declaring-class provenance for const-expr FCCs, so we recover it by
	 * instrumenting ReflectionAttribute. When the engine exposes it natively
	 * (ReflectionFunction::getConstExprClass, the serializable-closures patch),
	 * the engine-id path resolves cross-class FCCs directly and this is left
	 * off. There is no INI knob: it is simply how deepclone behaves on a build
	 * without native provenance. */
	zend_class_entry *rf_ce = zend_hash_str_find_ptr(CG(class_table),
		"reflectionfunction", sizeof("reflectionfunction") - 1);
	bool native_provenance = rf_ce && zend_hash_str_exists(&rf_ce->function_table,
		"getconstexprclass", sizeof("getconstexprclass") - 1);
	DC_G(capture_attribute_closures) = !native_provenance;

	if (DC_G(capture_attribute_closures)) {
		/* reflection is a required dependency, so its classes exist. The
		 * closures frameworks read travel through getArguments() (raw values)
		 * or newInstance() (as attribute-instance properties); hook both. */
		zend_class_entry *attr_ce = zend_hash_str_find_ptr(CG(class_table),
			"reflectionattribute", sizeof("reflectionattribute") - 1);
		if (attr_ce) {
			zend_function *fn = zend_hash_str_find_ptr(&attr_ce->function_table,
				"getarguments", sizeof("getarguments") - 1);
			if (fn && fn->type == ZEND_INTERNAL_FUNCTION) {
				dc_orig_attr_get_arguments = fn->internal_function.handler;
				fn->internal_function.handler = dc_attr_get_arguments_wrapper;
			}
			fn = zend_hash_str_find_ptr(&attr_ce->function_table,
				"newinstance", sizeof("newinstance") - 1);
			if (fn && fn->type == ZEND_INTERNAL_FUNCTION) {
				dc_orig_attr_new_instance = fn->internal_function.handler;
				fn->internal_function.handler = dc_attr_new_instance_wrapper;
			}
		}
	}
#endif

	return SUCCESS;
}

PHP_MSHUTDOWN_FUNCTION(deepclone)
{
#if PHP_VERSION_ID >= 80500
	/* Restore the original handlers if we replaced them. reflection shuts down
	 * after us (we depend on it), so its class table is still valid here. */
	if (dc_orig_attr_get_arguments || dc_orig_attr_new_instance) {
		zend_class_entry *attr_ce = zend_hash_str_find_ptr(CG(class_table),
			"reflectionattribute", sizeof("reflectionattribute") - 1);
		if (attr_ce) {
			zend_function *fn = zend_hash_str_find_ptr(&attr_ce->function_table,
				"getarguments", sizeof("getarguments") - 1);
			if (fn && fn->type == ZEND_INTERNAL_FUNCTION
					&& fn->internal_function.handler == dc_attr_get_arguments_wrapper) {
				fn->internal_function.handler = dc_orig_attr_get_arguments;
			}
			fn = zend_hash_str_find_ptr(&attr_ce->function_table,
				"newinstance", sizeof("newinstance") - 1);
			if (fn && fn->type == ZEND_INTERNAL_FUNCTION
					&& fn->internal_function.handler == dc_attr_new_instance_wrapper) {
				fn->internal_function.handler = dc_orig_attr_new_instance;
			}
		}
		dc_orig_attr_get_arguments = NULL;
		dc_orig_attr_new_instance = NULL;
	}
#endif

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
	/* attr_provenance is a persistent, cross-request cache initialized lazily
	 * on first capture and freed in GSHUTDOWN. capture_attribute_closures is
	 * decided at MINIT by whether the engine exposes native provenance. */
	memset(&deepclone_globals->attr_provenance, 0, sizeof(HashTable));
	deepclone_globals->capture_attribute_closures = 0;
}

static PHP_GSHUTDOWN_FUNCTION(deepclone)
{
	zend_hash_destroy(&deepclone_globals->hydrate_cache);
	if (deepclone_globals->attr_provenance.nTableSize) {
		zend_hash_destroy(&deepclone_globals->attr_provenance);
	}
}

#if PHP_VERSION_ID >= 80400 && PHP_VERSION_ID < 80600
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
	/* attr_provenance is NOT cleared here: it is a persistent, per-worker
	 * cache that survives across requests (freed in GSHUTDOWN). */
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
	PHP_MSHUTDOWN(deepclone),
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
