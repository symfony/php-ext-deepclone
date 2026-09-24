/* Project-local C++ libFuzzer target for deepclone_from_array().
 *
 * This links against PHP's fuzzer SAPI, loads deepclone as a normal extension,
 * and uses LLVM's FuzzedDataProvider to translate every byte stream into a
 * PHP array payload. Most class-table entries are selected from fixed PHP
 * class names.
 */

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <fuzzer/FuzzedDataProvider.h>

#include "Zend/zend.h"
#include "Zend/zend_exceptions.h"
#include "Zend/zend_interfaces.h"
#include "main/php.h"

extern "C" {
#include "fuzzer-sapi.h"
}

#define FUZZ_MAX_INPUT 8192
#define FUZZ_MAX_DEPTH 4
#define FUZZ_MAX_NODES 128

struct fuzz_input {
	FuzzedDataProvider provider;
	unsigned nodes;

	fuzz_input(const uint8_t *data, size_t size) : provider(data, size), nodes(0) {}

	uint8_t choose(uint8_t maximum)
	{
		return provider.ConsumeIntegralInRange<uint8_t>(0, maximum);
	}

	bool boolean()
	{
		return provider.ConsumeBool();
	}

	bool one_in(uint8_t denominator)
	{
		return choose(denominator - 1) == 0;
	}
};

typedef struct {
	zval value;
	zval mask;
	bool marked;
} fuzz_pair;

static zend_long fuzz_long(fuzz_input *in)
{
	return in->provider.ConsumeIntegral<zend_long>();
}

static zend_long fuzz_small_id(fuzz_input *in)
{
	static const zend_long ids[] = {
		ZEND_LONG_MIN, -9, -3, -2, -1, 0, 1, 2, 3, 9, ZEND_LONG_MAX,
	};
	return in->provider.PickValueInArray(ids);
}

static const char *fuzz_class_name(fuzz_input *in)
{
	static const char *const names[] = {
		"stdClass",
		"DeepCloneFuzzNode",
		"DeepCloneFuzzChild",
		"DeepCloneFuzzUnserialize",
		"DeepCloneFuzzWakeup",
		"DeepCloneFuzzConst",
		"Exception",
		"ArrayObject",
		"ArrayIterator",
		"SplObjectStorage",
		"DateTime",
		"BcMath\\Number",
		"DeepClone\\HydrationContext",
		"NoSuchDeepCloneFuzzClass",
	};
	return in->provider.PickValueInArray(names);
}

static const char *fuzz_scope_name(fuzz_input *in)
{
	static const char *const names[] = {
		"stdClass",
		"DeepCloneFuzzNode",
		"DeepCloneFuzzParent",
		"DeepCloneFuzzChild",
		"DeepCloneFuzzUnserialize",
		"Exception",
		"NoSuchDeepCloneFuzzScope",
	};
	return in->provider.PickValueInArray(names);
}

static const char *fuzz_value_string(fuzz_input *in)
{
	static const char *const strings[] = {
		"", "x", "v", "w", "cb", "secret", "message", "trace",
		"strlen", "strrev", "trim", "method", "named", "hidden", "no_such_function_xyz",
		"stdClass", "DeepCloneFuzzNode", "NoSuchDeepCloneFuzzClass",
		"Random\\IntervalBoundary::ClosedOpen",
		"DeepCloneFuzzEnum::Present", "DeepCloneFuzzEnum::Missing",
	};
	return in->provider.PickValueInArray(strings);
}

static void fuzz_make_native_serialized_class(fuzz_input *in, zval *out)
{
	static const char *const forms[] = {
		"O:8:\"stdClass\":0:{}",
		"O:8:\"stdClass\":1:{s:1:\"v\";i:7;}",
		"O:17:\"DeepCloneFuzzNode\":0:{}",
		"O:24:\"DeepCloneFuzzUnserialize\":1:{s:1:\"v\";i:7;}",
		"C:25:\"DeepCloneFuzzSerializable\":0:{}",
		"C:25:\"DeepCloneFuzzSerializable\":7:{payload}",
		"O:24:\"NoSuchDeepCloneFuzzClass\":0:{}",
		"O:8:\"stdClass\":1:{s:1:\"v\";}",
		"a:1:{i:0;i:1;}",
	};
	ZVAL_STRING(out, in->provider.PickValueInArray(forms));
}

static void fuzz_pair_init(fuzz_pair *pair)
{
	ZVAL_UNDEF(&pair->value);
	ZVAL_UNDEF(&pair->mask);
	pair->marked = false;
}

static void fuzz_add_index(zval *array, zend_ulong index, zval *value)
{
	zend_hash_index_update(Z_ARRVAL_P(array), index, value);
	ZVAL_UNDEF(value);
}

static void fuzz_add_next(zval *array, zval *value)
{
	zend_hash_next_index_insert(Z_ARRVAL_P(array), value);
	ZVAL_UNDEF(value);
}

static void fuzz_add_key(zval *array, const char *key, zval *value)
{
	zend_hash_str_update(Z_ARRVAL_P(array), key, strlen(key), value);
	ZVAL_UNDEF(value);
}

static zend_string *fuzz_property_name(fuzz_input *in)
{
	static const char *const names[] = {
		"x", "v", "w", "cb", "secret", "missing", "message", "trace", "42",
	};
	if (!in->one_in(4)) {
		const char *name = in->provider.PickValueInArray(names);
		return zend_string_init(name, strlen(name), 0);
	}

	std::string bytes = in->provider.ConsumeRandomLengthString(8);
	return zend_string_init(bytes.data(), bytes.size(), 0);
}

static void fuzz_make_scalar(fuzz_input *in, zval *out)
{
	switch (in->choose(5)) {
		case 0: ZVAL_NULL(out); break;
		case 1: ZVAL_BOOL(out, in->boolean()); break;
		case 2: ZVAL_LONG(out, fuzz_small_id(in)); break;
		case 3: ZVAL_LONG(out, fuzz_long(in)); break;
		case 4: ZVAL_STRING(out, fuzz_value_string(in)); break;
		default: {
			std::string bytes = in->provider.ConsumeRandomLengthString(16);
			ZVAL_STRINGL(out, bytes.data(), bytes.size());
			break;
		}
	}
}

static void fuzz_make_pair(fuzz_input *in, fuzz_pair *pair, unsigned depth);

static void fuzz_make_named_closure(fuzz_input *in, zval *value)
{
	array_init_size(value, 3);
	uint8_t target_kind = in->choose(4);
	zval target;
	if (target_kind == 0) {
		ZVAL_NULL(&target);
	} else if (target_kind == 1) {
		ZVAL_STRING(&target, fuzz_class_name(in));
	} else if (target_kind == 2) {
		ZVAL_LONG(&target, fuzz_small_id(in));
	} else if (target_kind == 3) {
		fuzz_make_scalar(in, &target);
	} else {
		zval private_target, private_name;
		array_init_size(&target, 2);
		ZVAL_LONG(&private_target, fuzz_small_id(in));
		ZVAL_STRING(&private_name, fuzz_value_string(in));
		fuzz_add_next(&target, &private_target);
		fuzz_add_next(&target, &private_name);
	}
	fuzz_add_next(value, &target);

	zval name;
	if (in->one_in(4)) {
		fuzz_make_scalar(in, &name);
	} else {
		ZVAL_STRING(&name, fuzz_value_string(in));
	}
	fuzz_add_next(value, &name);

	if (target_kind == 4 || in->one_in(8)) {
		zval method;
		ZVAL_STRING(&method, fuzz_value_string(in));
		fuzz_add_next(value, &method);
	}
}

static void fuzz_make_constexpr(fuzz_input *in, zval *value)
{
	array_init_size(value, 5);
	zval item;
	ZVAL_STRING(&item, !in->one_in(4) ? "DeepCloneFuzzConst" : fuzz_class_name(in));
	fuzz_add_next(value, &item);
	ZVAL_STRING(&item, in->boolean() ? "CALLBACK" : fuzz_value_string(in));
	fuzz_add_next(value, &item);
	if (in->boolean()) {
		ZVAL_NULL(&item);
	} else {
		ZVAL_LONG(&item, fuzz_small_id(in));
	}
	fuzz_add_next(value, &item);
	ZVAL_LONG(&item, fuzz_small_id(in));
	fuzz_add_next(value, &item);
	ZVAL_LONG(&item, !in->one_in(4) ? 2 : fuzz_small_id(in));
	fuzz_add_next(value, &item);
}

static void fuzz_make_pair(fuzz_input *in, fuzz_pair *pair, unsigned depth)
{
	fuzz_pair_init(pair);
	if (++in->nodes > FUZZ_MAX_NODES) {
		ZVAL_NULL(&pair->value);
		return;
	}

	uint8_t kind = in->choose(9);
	if (depth >= FUZZ_MAX_DEPTH && kind >= 6) {
		kind %= 6;
	}

	switch (kind) {
		case 0:
			fuzz_make_scalar(in, &pair->value);
			if (in->one_in(8)) {
				fuzz_make_scalar(in, &pair->mask); /* Unknown markers are inert. */
				pair->marked = true;
			}
			break;

		case 1:
			if (!in->one_in(4)) {
				ZVAL_LONG(&pair->value, fuzz_small_id(in));
			} else {
				fuzz_make_scalar(in, &pair->value);
			}
			ZVAL_TRUE(&pair->mask);
			pair->marked = true;
			break;

		case 2:
			if (!in->one_in(4)) {
				ZVAL_LONG(&pair->value, fuzz_small_id(in));
			} else {
				fuzz_make_scalar(in, &pair->value);
			}
			ZVAL_FALSE(&pair->mask);
			pair->marked = true;
			break;

		case 3:
			fuzz_make_named_closure(in, &pair->value);
			ZVAL_LONG(&pair->mask, 0);
			pair->marked = true;
			break;

		case 4:
			if (!in->one_in(4)) {
				ZVAL_STRING(&pair->value, fuzz_value_string(in));
			} else {
				fuzz_make_scalar(in, &pair->value);
			}
			ZVAL_STRINGL(&pair->mask, "e", 1);
			pair->marked = true;
			break;

		case 5:
			if (!in->one_in(4)) {
				fuzz_make_constexpr(in, &pair->value);
			} else {
				fuzz_make_scalar(in, &pair->value);
			}
			ZVAL_LONG(&pair->mask, 1);
			pair->marked = true;
			break;

		case 6: {
			array_init(&pair->value);
			array_init(&pair->mask);
			pair->marked = true;
			unsigned count = in->choose(4);
			for (unsigned i = 0; i < count; i++) {
				fuzz_pair child;
				fuzz_make_pair(in, &child, depth + 1);
				zend_ulong index = !in->one_in(4) ? i : (zend_ulong) fuzz_small_id(in);
				fuzz_add_index(&pair->value, index, &child.value);
				if (child.marked) {
					fuzz_add_index(&pair->mask, index, &child.mask);
				} else if (in->boolean()) {
					zval no_marker;
					ZVAL_NULL(&no_marker);
					fuzz_add_index(&pair->mask, index, &no_marker);
				}
			}
			if (in->one_in(8)) {
				zval extra;
				ZVAL_TRUE(&extra);
				fuzz_add_index(&pair->mask, 99, &extra);
			}
			break;
		}

		case 7:
			fuzz_make_scalar(in, &pair->value);
			array_init(&pair->mask);
			pair->marked = true;
			break;

		case 8:
			fuzz_make_scalar(in, &pair->value);
			ZVAL_NEW_REF(&pair->value, &pair->value);
			if (in->boolean()) {
				ZVAL_FALSE(&pair->mask);
				pair->marked = true;
			}
			break;

		default:
			array_init(&pair->value);
			for (unsigned i = 0, count = in->choose(3); i < count; i++) {
				zval item;
				fuzz_make_scalar(in, &item);
				fuzz_add_next(&pair->value, &item);
			}
			break;
	}
}

static void fuzz_make_classes(fuzz_input *in, zval *classes, uint8_t scenario, uint32_t *class_count)
{
	if (scenario == 0) {
		ZVAL_EMPTY_STRING(classes);
		*class_count = 0;
		return;
	}

	if (scenario == 4) {
		array_init(classes);
		unsigned count = 1 + in->choose(3);
		for (unsigned i = 0; i < count; i++) {
			zval name;
			if (in->boolean()) {
				ZVAL_STRING(&name, fuzz_class_name(in));
			} else {
				fuzz_make_scalar(in, &name);
				/* deepclone treats every string whose second byte is ':' as a
				 * native serialized object. Keep boundary-name mutations in this
				 * extension-only target, but make that dispatch unreachable. */
				if (Z_TYPE(name) == IS_STRING && Z_STRLEN(name) > 1 && Z_STRVAL(name)[1] == ':') {
					Z_STRVAL(name)[1] = '_';
				}
			}
			fuzz_add_next(classes, &name);
		}
		*class_count = count;
		return;
	}

	if (scenario == 8) {
		/* One dedicated scenario emits a small set of structured native
		 * serialization strings so the extension's native-object handoff is
		 * covered without exposing php_var_unserialize() to arbitrary byte
		 * strings. */
		array_init(classes);
		unsigned count = 1 + in->choose(2);
		for (unsigned i = 0; i < count; i++) {
			zval serialized;
			fuzz_make_native_serialized_class(in, &serialized);
			fuzz_add_next(classes, &serialized);
		}
		*class_count = count;
		return;
	}

	static const char *const useful[] = {
		"DeepCloneFuzzNode", "DeepCloneFuzzUnserialize", "DeepCloneFuzzWakeup",
		"DeepCloneFuzzChild", "stdClass", "Exception", "ArrayObject",
	};
	array_init_size(classes, sizeof(useful) / sizeof(useful[0]));
	for (unsigned i = 0; i < sizeof(useful) / sizeof(useful[0]); i++) {
		zval name;
		ZVAL_STRING(&name, useful[i]);
		fuzz_add_next(classes, &name);
	}
	*class_count = sizeof(useful) / sizeof(useful[0]);
}

static uint32_t fuzz_make_meta(fuzz_input *in, zval *meta, uint8_t scenario, uint32_t class_count)
{
	uint32_t count = scenario == 0 ? 0 : 1 + in->choose(4);
	if (scenario == 4 && in->boolean()) {
		fuzz_make_scalar(in, meta);
		return count;
	}

	array_init_size(meta, count);
	for (uint32_t id = 0; id < count; id++) {
		zval entry;
		if (scenario == 4 && in->one_in(4)) {
			fuzz_make_scalar(in, &entry);
		} else {
			array_init_size(&entry, 2);
			zval cidx, replay;
			ZVAL_LONG(&cidx, class_count ? in->provider.ConsumeIntegralInRange<uint32_t>(0, class_count - 1) : fuzz_small_id(in));
			if (scenario == 3) {
				ZVAL_LONG(&replay, -1);
			} else {
				static const zend_long flags[] = {-1, 0, 0, 0, 1};
				ZVAL_LONG(&replay, in->provider.PickValueInArray(flags));
			}
			fuzz_add_next(&entry, &cidx);
			fuzz_add_next(&entry, &replay);
		}
		zend_ulong index = scenario == 4 && in->one_in(4) ? fuzz_small_id(in) : id;
		fuzz_add_index(meta, index, &entry);
	}
	return count;
}

static void fuzz_make_properties(fuzz_input *in, uint32_t object_count, zval *properties, zval *resolve)
{
	array_init(properties);
	array_init(resolve);
	unsigned scopes = 1 + in->choose(2);
	for (unsigned s = 0; s < scopes; s++) {
		const char *scope_name = fuzz_scope_name(in);
		zval scope_values, scope_masks;
		array_init(&scope_values);
		array_init(&scope_masks);

		unsigned names = 1 + in->choose(3);
		for (unsigned p = 0; p < names; p++) {
			zend_string *name = fuzz_property_name(in);
			zval id_values, id_masks;
			array_init(&id_values);
			array_init(&id_masks);
			unsigned entries = 1 + in->choose(3);
			for (unsigned j = 0; j < entries; j++) {
				fuzz_pair pair;
				fuzz_make_pair(in, &pair, 0);
				zend_ulong id = !in->one_in(4) && object_count
					? in->provider.ConsumeIntegralInRange<uint32_t>(0, object_count - 1)
					: fuzz_small_id(in);
				fuzz_add_index(&id_values, id, &pair.value);
				if (pair.marked) {
					fuzz_add_index(&id_masks, id, &pair.mask);
				}
			}
			zend_hash_update(Z_ARRVAL(scope_values), name, &id_values);
			ZVAL_UNDEF(&id_values);
			if (zend_hash_num_elements(Z_ARRVAL(id_masks))) {
				zend_hash_update(Z_ARRVAL(scope_masks), name, &id_masks);
				ZVAL_UNDEF(&id_masks);
			} else {
				zval_ptr_dtor(&id_masks);
			}
			zend_string_release(name);
		}
		fuzz_add_key(properties, scope_name, &scope_values);
		if (zend_hash_num_elements(Z_ARRVAL(scope_masks))) {
			fuzz_add_key(resolve, scope_name, &scope_masks);
		} else {
			zval_ptr_dtor(&scope_masks);
		}
	}
}

static void fuzz_make_refs(fuzz_input *in, zval *refs, zval *masks)
{
	array_init(refs);
	array_init(masks);
	unsigned count = 1 + in->choose(4);
	for (unsigned i = 1; i <= count; i++) {
		fuzz_pair pair;
		fuzz_make_pair(in, &pair, 0);
		zend_ulong id = !in->one_in(4) ? i : (zend_ulong) fuzz_small_id(in);
		fuzz_add_index(refs, id, &pair.value);
		if (pair.marked) {
			fuzz_add_index(masks, id, &pair.mask);
		}
	}
}

static void fuzz_make_states(fuzz_input *in, uint32_t object_count, zval *states)
{
	array_init(states);
	unsigned count = 1 + in->choose(4);
	for (unsigned i = 0; i < count; i++) {
		zval state;
		if (in->boolean()) {
			ZVAL_LONG(&state, object_count
				? in->provider.ConsumeIntegralInRange<uint32_t>(0, object_count - 1)
				: fuzz_small_id(in));
		} else {
			array_init_size(&state, 3);
			zval id;
			ZVAL_LONG(&id, object_count && !in->one_in(4)
				? in->provider.ConsumeIntegralInRange<uint32_t>(0, object_count - 1)
				: fuzz_small_id(in));
			fuzz_add_next(&state, &id);
			fuzz_pair pair;
			fuzz_make_pair(in, &pair, 0);
			fuzz_add_next(&state, &pair.value);
			if (pair.marked) {
				fuzz_add_next(&state, &pair.mask);
			}
		}
		fuzz_add_next(states, &state);
	}
}

static void fuzz_make_allowed(fuzz_input *in, zval *allowed, uint8_t scenario)
{
	/* A non-NULL allow-list rejects serialized class-table entries before the
	 * native parser is reached. Keep this scenario on the handoff path; the
	 * ordinary scenarios continue fuzzing the allow-list grammar itself. */
	if (scenario == 8 || in->one_in(4)) {
		ZVAL_NULL(allowed);
		return;
	}
	array_init(allowed);
	unsigned count = in->choose(4);
	for (unsigned i = 0; i < count; i++) {
		zval name;
		if (in->one_in(8)) {
			fuzz_make_scalar(in, &name);
		} else if (in->one_in(4)) {
			ZVAL_STRING(&name, "Closure");
		} else {
			ZVAL_STRING(&name, fuzz_class_name(in));
		}
		fuzz_add_next(allowed, &name);
	}
}

static void fuzz_make_forced_lazy_state(fuzz_input *in, zval *payload, zval *allowed, zval *allow_named)
{
	array_init(payload);
	zval item;
	ZVAL_STRING(&item, "DeepCloneFuzzUnserialize");
	fuzz_add_key(payload, "classes", &item);

	zval meta, entry;
	array_init_size(&meta, 1);
	array_init_size(&entry, 2);
	ZVAL_LONG(&item, 0);
	fuzz_add_next(&entry, &item);
	ZVAL_LONG(&item, -1);
	fuzz_add_next(&entry, &item);
	fuzz_add_index(&meta, 0, &entry);
	fuzz_add_key(payload, "objectMeta", &meta);

	ZVAL_LONG(&item, 0);
	fuzz_add_key(payload, "prepared", &item);
	ZVAL_TRUE(&item);
	fuzz_add_key(payload, "mask", &item);

	zval states, state, state_value, state_mask, cexpr;
	array_init_size(&states, 1);
	array_init_size(&state, 3);
	ZVAL_LONG(&item, 0);
	fuzz_add_next(&state, &item);
	array_init_size(&state_value, 1);
	array_init_size(&cexpr, 5);
	ZVAL_STRING(&item, "DeepCloneFuzzConst");
	fuzz_add_next(&cexpr, &item);
	ZVAL_STRING(&item, in->one_in(4) ? fuzz_value_string(in) : "CALLBACK");
	fuzz_add_next(&cexpr, &item);
	ZVAL_NULL(&item);
	fuzz_add_next(&cexpr, &item);
	ZVAL_LONG(&item, in->one_in(4) ? fuzz_small_id(in) : 0);
	fuzz_add_next(&cexpr, &item);
	ZVAL_LONG(&item, in->one_in(4) ? fuzz_small_id(in) : 7);
	fuzz_add_next(&cexpr, &item);
	fuzz_add_key(&state_value, "cb", &cexpr);
	fuzz_add_next(&state, &state_value);
	array_init_size(&state_mask, 1);
	ZVAL_LONG(&item, 1);
	fuzz_add_key(&state_mask, "cb", &item);
	fuzz_add_next(&state, &state_mask);
	fuzz_add_next(&states, &state);
	fuzz_add_key(payload, "states", &states);

	if (in->boolean()) {
		ZVAL_NULL(allowed);
	} else {
		array_init_size(allowed, 2);
		ZVAL_STRING(&item, "DeepCloneFuzzUnserialize");
		fuzz_add_next(allowed, &item);
		ZVAL_STRING(&item, "Closure");
		fuzz_add_next(allowed, &item);
	}
	ZVAL_FALSE(allow_named);
}

static void fuzz_make_forced_callable(fuzz_input *in, zval *payload, zval *allowed, zval *allow_named)
{
	array_init(payload);
	zval classes, item;
	array_init_size(&classes, 2);
	ZVAL_STRING(&item, "DeepCloneFuzzNode");
	fuzz_add_next(&classes, &item);
	ZVAL_STRING(&item, "stdClass");
	fuzz_add_next(&classes, &item);
	fuzz_add_key(payload, "classes", &classes);

	zval meta;
	array_init_size(&meta, 2);
	for (zend_long id = 0; id < 2; id++) {
		zval entry;
		array_init_size(&entry, 2);
		ZVAL_LONG(&item, id);
		fuzz_add_next(&entry, &item);
		ZVAL_LONG(&item, 0);
		fuzz_add_next(&entry, &item);
		fuzz_add_index(&meta, id, &entry);
	}
	fuzz_add_key(payload, "objectMeta", &meta);
	ZVAL_LONG(&item, 1);
	fuzz_add_key(payload, "prepared", &item);
	ZVAL_TRUE(&item);
	fuzz_add_key(payload, "mask", &item);

	zval callable;
	array_init_size(&callable, 3);
	switch (in->choose(2)) {
		case 0:
			ZVAL_LONG(&item, 0);
			fuzz_add_next(&callable, &item);
			ZVAL_STRING(&item, "method");
			fuzz_add_next(&callable, &item);
			break;
		case 1:
			ZVAL_STRING(&item, "DeepCloneFuzzNode");
			fuzz_add_next(&callable, &item);
			ZVAL_STRING(&item, "named");
			fuzz_add_next(&callable, &item);
			break;
		default: {
			zval private_callable;
			array_init_size(&private_callable, 2);
			ZVAL_LONG(&item, 0);
			fuzz_add_next(&private_callable, &item);
			ZVAL_STRING(&item, "hidden");
			fuzz_add_next(&private_callable, &item);
			fuzz_add_next(&callable, &private_callable);
			ZVAL_STRING(&item, "DeepCloneFuzzNode");
			fuzz_add_next(&callable, &item);
			ZVAL_STRING(&item, "hidden");
			fuzz_add_next(&callable, &item);
			break;
		}
	}
	zval properties, scope, ids;
	array_init_size(&properties, 1);
	array_init_size(&scope, 1);
	array_init_size(&ids, 1);
	fuzz_add_index(&ids, 1, &callable);
	fuzz_add_key(&scope, "cb", &ids);
	fuzz_add_key(&properties, "stdClass", &scope);
	fuzz_add_key(payload, "properties", &properties);
	zval resolve;
	array_init_size(&resolve, 1);
	array_init_size(&scope, 1);
	array_init_size(&ids, 1);
	ZVAL_LONG(&item, 0);
	fuzz_add_index(&ids, 1, &item);
	fuzz_add_key(&scope, "cb", &ids);
	fuzz_add_key(&resolve, "stdClass", &scope);
	fuzz_add_key(payload, "resolve", &resolve);
	ZVAL_NULL(allowed);
	ZVAL_TRUE(allow_named);
}

static void fuzz_make_payload(fuzz_input *in, zval *payload, zval *allowed, zval *allow_named)
{
	/* Scenarios 0-10 emphasize, respectively: the top-level value, properties,
	 * references, replayed state, malformed metadata, all optional tables,
	 * cross-table relationships, an unconstrained graph, native-object handoff,
	 * lazy state, and named callables. */
	uint8_t scenario = in->choose(10);
	if (scenario == 9) {
		fuzz_make_forced_lazy_state(in, payload, allowed, allow_named);
		return;
	}
	if (scenario == 10) {
		fuzz_make_forced_callable(in, payload, allowed, allow_named);
		return;
	}
	array_init(payload);

	zval classes, meta;
	uint32_t class_count;
	fuzz_make_classes(in, &classes, scenario, &class_count);
	uint32_t object_count = fuzz_make_meta(in, &meta, scenario, class_count);
	fuzz_add_key(payload, "classes", &classes);
	fuzz_add_key(payload, "objectMeta", &meta);

	fuzz_pair top;
	fuzz_make_pair(in, &top, 0);
	if ((scenario == 1 || scenario == 3 || scenario == 6 || scenario == 8) && object_count) {
		zval_ptr_dtor(&top.value);
		if (top.marked) zval_ptr_dtor(&top.mask);
		ZVAL_LONG(&top.value, 0);
		ZVAL_TRUE(&top.mask);
		top.marked = true;
	}
	fuzz_add_key(payload, "prepared", &top.value);
	if (top.marked) fuzz_add_key(payload, "mask", &top.mask);

	if (scenario == 1 || scenario == 5 || scenario == 6 || scenario == 8 || in->boolean()) {
		zval properties, resolve;
		fuzz_make_properties(in, object_count, &properties, &resolve);
		fuzz_add_key(payload, "properties", &properties);
		if (zend_hash_num_elements(Z_ARRVAL(resolve)) || in->one_in(4)) {
			fuzz_add_key(payload, "resolve", &resolve);
		} else {
			zval_ptr_dtor(&resolve);
		}
	}

	if (scenario == 2 || scenario == 5 || scenario == 6 || in->boolean()) {
		zval refs, ref_masks;
		fuzz_make_refs(in, &refs, &ref_masks);
		fuzz_add_key(payload, "refs", &refs);
		if (zend_hash_num_elements(Z_ARRVAL(ref_masks)) || in->one_in(4)) {
			fuzz_add_key(payload, "refMasks", &ref_masks);
		} else {
			zval_ptr_dtor(&ref_masks);
		}
	}

	if (scenario == 3 || scenario == 5 || in->boolean()) {
		zval states;
		fuzz_make_states(in, object_count, &states);
		fuzz_add_key(payload, "states", &states);
	}

	fuzz_make_allowed(in, allowed, scenario);
	ZVAL_BOOL(allow_named, in->boolean());
}

static const char FUZZ_FIXTURES[] =
	"class DeepCloneFuzzParent { private $secret = null; protected $v = null; }\n"
	"class DeepCloneFuzzChild extends DeepCloneFuzzParent { public $cb = null; public $w = null; }\n"
	"class DeepCloneFuzzNode { public $v = null; public $w = null; public $cb = null; private $secret = null; public function method() { return 1; } public static function named() { return 2; } private function hidden() { return 3; } }\n"
	"class DeepCloneFuzzUnserialize { public $cb = null; public $state = null; public function __unserialize(array $state): void { $this->state = $state; } }\n"
	"class DeepCloneFuzzWakeup { public $cb = null; public $woke = false; public function __wakeup(): void { $this->woke = true; } }\n"
	"class DeepCloneFuzzSerializable implements Serializable { public $state = null; public function serialize(): string { return (string) $this->state; } public function unserialize(string $data): void { $this->state = $data; } }\n"
	"class DeepCloneFuzzConst { public const CALLBACK = static function () { return 1; }; }\n"
	"enum DeepCloneFuzzEnum { case Present; public const Alias = self::Present; }\n"
	"function deepCloneFuzzInvoke($data, $allowed, $allowNamed) { $value = deepclone_from_array($data, $allowed, $allowNamed); if ($value instanceof DeepCloneFuzzUnserialize) { try { $value->state; } catch (Throwable) {} } return $value; }\n";

extern "C" int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
	if (size == 0 || size > FUZZ_MAX_INPUT) {
		return 0;
	}
	if (fuzzer_request_startup() == FAILURE) {
		return 0;
	}

	fuzzer_setup_dummy_frame();
	zend_eval_stringl(FUZZ_FIXTURES, sizeof(FUZZ_FIXTURES) - 1, NULL, "deepclone-fuzzer-fixtures");
	if (EG(exception)) {
		fuzzer_request_shutdown();
		return 0;
	}

	fuzz_input in(data, size);
	zval args[3], retval;
	ZVAL_UNDEF(&args[0]);
	ZVAL_UNDEF(&args[1]);
	ZVAL_UNDEF(&args[2]);
	ZVAL_UNDEF(&retval);
	fuzz_make_payload(&in, &args[0], &args[1], &args[2]);
	if (getenv("DEEPCLONE_FUZZ_DUMP")) {
		fputs("payload:\n", stderr);
		zend_string *dump = zend_print_zval_r_to_str(&args[0], 0);
		fwrite(ZSTR_VAL(dump), ZSTR_LEN(dump), 1, stderr);
		zend_string_release(dump);
		fputs("\nallowed_classes:\n", stderr);
		dump = zend_print_zval_r_to_str(&args[1], 0);
		fwrite(ZSTR_VAL(dump), ZSTR_LEN(dump), 1, stderr);
		zend_string_release(dump);
		fprintf(stderr, "\nallow_named_closures: %s\n", zend_is_true(&args[2]) ? "true" : "false");
		zval_ptr_dtor(&args[2]);
		zval_ptr_dtor(&args[1]);
		zval_ptr_dtor(&args[0]);
		fuzzer_request_shutdown();
		return 0;
	}

	zend_function *function = reinterpret_cast<zend_function *>(zend_hash_str_find_ptr(
		CG(function_table), "deepclonefuzzinvoke", sizeof("deepclonefuzzinvoke") - 1));
	if (function) {
		zend_try {
			zend_call_known_function(function, NULL, NULL, &retval, 3, args, NULL);
		} zend_end_try();
	}

	if (!Z_ISUNDEF(retval)) zval_ptr_dtor(&retval);
	zval_ptr_dtor(&args[2]);
	zval_ptr_dtor(&args[1]);
	zval_ptr_dtor(&args[0]);
	fuzzer_request_shutdown();
	return 0;
}

extern "C" int LLVMFuzzerInitialize(int *argc, char ***argv)
{
	const char *module = getenv("DEEPCLONE_FUZZ_EXTENSION");
	if (!module || !*module) {
		module = DEEPCLONE_FUZZ_EXTENSION;
	}
	char ini[4096];
	if (snprintf(ini, sizeof(ini), "extension=%s", module) >= (int) sizeof(ini)) {
		fprintf(stderr, "DEEPCLONE_FUZZ_EXTENSION is too long\n");
		exit(1);
	}
	if (fuzzer_init_php(ini) == FAILURE) {
		fprintf(stderr, "Failed to initialize PHP/deepclone\n");
		exit(1);
	}
	return 0;
}

/* ASan contains the LSan runtime on Linux; keep leak detection disabled even
 * when the caller forgets ASAN_OPTIONS=detect_leaks=0. */
#if defined(__has_feature)
# if __has_feature(address_sanitizer)
extern "C" const char *__asan_default_options(void) { return "detect_leaks=0:abort_on_error=1"; }
# endif
#endif

extern "C" const char *__ubsan_default_options(void) { return "halt_on_error=1:print_stacktrace=1"; }
