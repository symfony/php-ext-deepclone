# Changelog

All notable changes to this extension will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [0.5.0] - 2026-04-16

### BC Break

- `deepclone_hydrate()` now interprets `$vars` exclusively as a flat
  mangled-key array (the shape `(array) $obj` produces). The per-class
  scoped shape (`[$class => ['prop' => $val]]`) is no longer supported —
  callers passing the old shape will hit the `"invalid mangled key"` /
  `"not a parent"` errors on NUL-prefixed keys, or silently create a
  dynamic property named after the class on non-NUL keys. Migrate by
  flattening: for each scope entry, use bare names for public / protected
  / most-derived-private, and `"\0ScopeClass\0prop"` for parent-private
  props. Motivation: the two shapes were functionally equivalent (same
  resolution path, same slot writes), and keeping both required an
  intermediate scoped_props HashTable + a double-pass write. Dropping
  scoped mode simplifies the dispatcher into a single key-parse + write
  loop, and removes ~200 lines of C.
- `DEEPCLONE_HYDRATE_MANGLED_VARS` constant removed — flat mangled is
  now the only mode, so the flag is redundant. Callers who were passing
  the flag can simply drop it.
- `DEEPCLONE_HYDRATE_PRESERVE_REFS` flag value changed from `1 << 3` to
  `1 << 2` (filling the slot vacated by `DEEPCLONE_HYDRATE_MANGLED_VARS`).
  Symbolic references via the constant name are unaffected; anyone using
  the raw integer value `4` now gets `PRESERVE_REFS` instead of the old
  `MANGLED_VARS` — in practice both are the flags real callers pass, so
  the arithmetic happens to line up.

### Fixed

- `deepclone_hydrate()` rejects the SPL-internal-state `"\0"` key on
  objects that don't support it (anything other than `SplObjectStorage`,
  `ArrayObject`, `ArrayIterator`) with a `ValueError`. Previously the
  value silently landed in `obj->properties` as a NUL-named dynamic
  property.
- `deepclone_hydrate()` rejects malformed SPL `"\0"` payloads: a
  non-even-count pair stream for `SplObjectStorage` and a payload with
  more than 3 ctor args for `ArrayObject` / `ArrayIterator`. Both were
  previously tolerated silently (odd tail dropped; excess args truncated).
- `deepclone_hydrate()` no longer direct-writes `IS_PROP_UNINIT` to a
  lazy object's slot via the `null` → uninitialized shortcut. The
  shortcut is now gated on `zend_lazy_object_initialized(obj)`, so
  `DEEPCLONE_HYDRATE_NO_LAZY_INIT` + lazy objects fall through to the
  Reflection-based path instead of bypassing the lazy-props bookkeeping.
- `deepclone_from_array()` cross-validates `objectMeta` wakeup flags
  against `states` entries: each state entry must match the sign
  advertised in `objectMeta[id][1]` (positive → `__wakeup`, negative →
  `__unserialize`), and any id flagged for state replay without a
  matching entry is rejected. Closes a validation hole where payloads
  with impossible meta like `[0, 999]` or `[0, -123]` were accepted.
- `deepclone_from_array()` routes writes to undeclared property names
  on non-stdClass objects through `zend_update_property_ex()` instead
  of `zend_std_write_property()`, respecting overridden `write_property`
  handlers on internal classes and extensions. Matches the
  `deepclone_hydrate()` path.
- `deepclone_from_array()` throws `ValueError` on out-of-range object
  ids in `"properties"` entries (previously silently skipped).

### Changed

- `deepclone_from_array()` object-creation loop drops the pointer-scan
  over `class_names[]` that recovered the class id per object. A
  per-object `uint32_t class_id` is stored directly from the
  `objectMeta` parse, turning an O(N × K) step into O(N) on payloads
  with many objects across many classes.
- `deepclone_hydrate()` caches the `offsetSet` method lookup across
  iterations on `SplObjectStorage` `"\0"` payloads (was re-resolved
  by name on every entry).

## [0.4.0] - 2026-04-15

### BC Break

- `deepclone_hydrate()` no longer preserves PHP `&` references from `$vars`
  onto the target property slots by default. Incoming reference zvals are
  dereferenced on write (`ZVAL_DEREF`), so property slots hold plain values
  instead of ref links. Pass the new `DEEPCLONE_HYDRATE_PRESERVE_REFS` flag
  in `$flags` to opt back into the old behavior. Motivation: the ref-preserving
  path requires a per-call probe of the input array, which dominated cost for
  typical DTO hydration; making it opt-in brings the polyfill in line with
  Reflection-based hydrators on ref-less input. Callers that intentionally
  share a value slot between two properties (or between a property and a
  caller-side variable) need to add the flag.

### Added

- `DEEPCLONE_HYDRATE_PRESERVE_REFS` constant — see BC break above. Composes
  with `DEEPCLONE_HYDRATE_MANGLED_VARS`, `DEEPCLONE_HYDRATE_CALL_HOOKS`, and
  `DEEPCLONE_HYDRATE_NO_LAZY_INIT`.

### Changed

- `deepclone_hydrate()` scoped-mode property-name validation now matches
  `unserialize()` permissiveness: integer keys coerce to strings on dynamic
  property access; NUL-in-middle names are stored as raw dynamic properties
  (same as `unserialize()` on an `O:…` payload with a NUL-containing key);
  NUL-prefix names surface the engine's native `Error: Cannot access property
  starting with "\0"`. The pre-v0.4.0 `ValueError` was stricter than
  `unserialize()` and cost a per-prop validation in the hot path; dropping it
  aligns the semantics and saves hot-path work. `DEEPCLONE_HYDRATE_MANGLED_VARS`
  mode still parses and validates mangled keys.

## [0.3.1] - 2026-04-15

### Fixed

- `deepclone_hydrate()` error messages for NUL-containing property names
  in scoped mode referenced the pre-v0.3.0 `$scoped_vars`/`$mangled_vars`
  parameters. Updated to point at `DEEPCLONE_HYDRATE_MANGLED_VARS` and
  the new `$flags` argument.

## [0.3.0] - 2026-04-15

### BC Break

- `deepclone_hydrate()` now takes a single `$vars` array instead of
  separate `$scoped_vars` and `$mangled_vars`. The default interpretation
  is the scoped per-class shape; pass the new `DEEPCLONE_HYDRATE_MANGLED_VARS`
  flag in `$flags` to interpret `$vars` as a flat mangled-key array (the
  shape `(array) $object` produces). Old positional callers
  (`deepclone_hydrate($obj, [], $mangled)`) need to be updated to
  `deepclone_hydrate($obj, $mangled, DEEPCLONE_HYDRATE_MANGLED_VARS)`.
  As a footgun guard, passing a NUL-prefixed key in scoped mode raises
  a `ValueError` pointing at the missing flag.

### Added

- `DEEPCLONE_HYDRATE_MANGLED_VARS` constant — see BC break above.

### Changed

- `deepclone_hydrate()` silently skips readonly writes when the target
  slot already holds an identical value (`===`). Avoids "Cannot modify
  readonly property" on idempotent rehydration. Writes to uninitialized
  readonly and to different-valued readonly still obey engine semantics.
- `deepclone_hydrate()` writes `null` into a non-nullable typed property
  as `unset()` (restoring the uninitialized state) instead of raising
  `TypeError`. Nullable/mixed types keep their existing semantics.
  Hooked properties are exempt (no backing slot to "unset"; the set
  hook may handle `null` itself).
- `deepclone_hydrate()` casts scalar values to the matching backed-enum
  case when the target is a single-type (possibly nullable) backed-enum
  property and the value matches the enum's backing type (`int` ↔ int-
  backed, `string` ↔ string-backed). Unknown backing values raise the
  standard `ValueError` from `Enum::from()`. Decision rests on the
  property type only — `DEEPCLONE_HYDRATE_CALL_HOOKS` and hook presence
  don't change it. Set hooks on enum-typed properties accordingly
  receive the enum case, not the raw scalar.

### Added

- `deepclone_hydrate(..., int $flags = 0)` — new optional parameter to
  choose the write semantics for declared-property assignments:
  - `DEEPCLONE_HYDRATE_CALL_HOOKS` — `ReflectionProperty::setValue`
    semantics: invoke user-defined set hooks on hooked properties.
  - `DEEPCLONE_HYDRATE_NO_LAZY_INIT` —
    `ReflectionProperty::setRawValueWithoutLazyInitialization` semantics:
    skip the lazy initializer for each written property; realize the
    object when the last lazy property is set. Delegated to the
    Reflection API because the engine helpers the method relies on
    (`zend_lazy_object_decr_lazy_props`, `zend_lazy_object_realize`) are
    not exported as `ZEND_API`.
  - Default (0) — `setRawValue` semantics (bypass set hooks, type-check).
  - The two flags are mutually exclusive; unknown bits are rejected with
    `ValueError`.
- `deepclone_from_array()` always uses the default setRawValue semantics
  (same policy as `unserialize()` — payload-driven).

## [0.2.0] - 2026-04-14

### Added

- `deepclone_hydrate(object|string $object_or_class, array $scoped_vars = [], array $mangled_vars = []): object` —
  instantiates a class (or takes an existing object) and sets its properties,
  including private, protected, and readonly ones. Handles mangled key formats
  (`"\0ClassName\0prop"`, `"\0*\0prop"`), SPL special cases (ArrayObject,
  ArrayIterator, SplObjectStorage via `"\0"` key), and preserves PHP `&`
  references with correct type source tracking for typed properties.
- Instantiability validation for `deepclone_hydrate`: rejects the same classes
  as `deepclone_from_array` (abstract, interface, trait, enum, anonymous,
  Reflector subclasses, internal classes without serialization API). Results
  are cached per class for zero-cost repeated calls.
- `ValueError` on invalid input: integer keys in `$mangled_vars`, non-array
  values in `$scoped_vars`, mangled keys inside `$scoped_vars`, property names
  containing NUL bytes, and scopes that aren't a parent of the object's class.
- Strict scope validation in `deepclone_from_array()`: rejects unloaded
  scope-class names, scopes that aren't a parent of the target object's
  class, stdClass-scoped writes targeting non-public declared properties,
  and non-stdClass scopes referencing property names not declared on the
  scope class. Blocks scope-confusion payloads that could otherwise reach
  private slots on unrelated classes that happen to share a property name.
- Mangled-key validation in `deepclone_hydrate()`: rejects keys with a
  missing second NUL separator (e.g. `"\0broken"`) or an empty class name
  (e.g. `"\0\0prop"`) with a `ValueError` instead of silently skipping.

### Changed

- All function parameters now use snake_case to follow PHP conventions:
  `$allowed_classes`, `$object_or_class`, `$scoped_vars`, `$mangled_vars`.
- `deepclone_from_array()` now writes declared properties via direct
  `OBJ_PROP` slot access (same fast path as `deepclone_hydrate`), including
  correct `zend_reference` type-source tracking for typed properties. On a
  50-node graph this is ~25% faster and also covers a latent assertion on
  references flowing through typed user-class properties.
- Non-virtual hooked properties (PHP 8.4+) are now written via direct slot
  access, bypassing the `set` hook. Matches `ReflectionProperty::setRawValue`
  semantics: hydration restores stored state rather than re-running
  transformation logic. Virtual properties still go through the engine
  write path (they have no backing slot).
- `deepclone_to_array()` scalar fast path in the transpose loop — ~10%
  faster on graphs dominated by scalar leaves.
- Scope-class resolution in `deepclone_from_array()` uses
  `zend_lookup_class_ex(..., ZEND_FETCH_CLASS_NO_AUTOLOAD)` — leverages the
  per-`zend_string` CE cache for O(1) repeat lookups and never triggers
  autoload for scope names (scope classes must already be loaded as parents
  of validated objects).

### Fixed

- `deepclone_to_array()` no longer warns about `__sleep()`-listed typed
  properties that are uninitialized — matching native `serialize()` behavior.
- `deepclone_from_array()` rejects ref-id values equal to `ZEND_LONG_MIN` or
  non-negative — prevents signed-integer negation UB on malformed payloads.
- ZTS thread-safety: the per-class instantiability cache used by
  `deepclone_hydrate()` is now per-thread via module globals (previously a
  function-level static, racy under concurrent ZTS init).

## [0.1.1] - 2026-04-10

### Fixed

- Memory leaks on objects with `__unserialize`: spurious `GC_TRY_ADDREF` on
  arrays transferred (not shared) into the states output.
- Assertion failure in debug builds: `dc_mask_cleanup` called
  `zend_hash_apply` on a shared (refcount > 1) mask array. Fixed with
  `SEPARATE_ARRAY` before iterating.

### Changed

- Replaced `class_list`, `ce_cache`, and `objects` HashTables in
  `deepclone_from_array()` with flat C arrays for lower overhead.
- Use `zend_hash_find_known_hash()` for all interned key lookups.
- Use `DC_MASK_IS_NAMED_CLOSURE()` consistently in `dc_mask_has_closure`.
- Added Serializable code path test (`deepclone_serializable.phpt`).
- CI: added PHP debug build job for Zend MM leak detection; enabled
  ASAN LeakSanitizer (`detect_leaks=1`).

## [0.1.0] - 2026-04-10

### Added

- `deepclone_to_array(mixed $value, ?array $allowedClasses = null): array` —
  walks a PHP value graph and produces a pure-array payload (only scalars and
  nested arrays). Compatible with the wire format used by
  `Symfony\Component\VarExporter\DeepCloner`.
- `deepclone_from_array(array $data, ?array $allowedClasses = null): mixed` —
  reconstructs a value graph from a payload produced by `deepclone_to_array()`.
- `$allowedClasses` parameter on both functions, matching `unserialize()`'s
  `allowed_classes` option: `null` = allow all, `[]` = allow none,
  case-insensitive. Closures require `"Closure"` in the list.
- Two typed exceptions under the `DeepClone\` namespace, both extending
  `\InvalidArgumentException`:
  - `DeepClone\NotInstantiableException` — thrown by `deepclone_to_array()`
    when the input contains a resource or a non-instantiable class.
  - `DeepClone\ClassNotFoundException` — thrown by `deepclone_from_array()`
    when the payload references a class that no longer exists.
- Human-friendly exception messages:
  `'Type "X" is not instantiable.'`, `'Class "X" not found.'`
- Rejects internal classes that hold hidden C-level state (custom
  `create_object` handler) and declare no serialization API. Final internal
  classes are probed via `object_init_ex()` — stateless ones (e.g.
  `MongoDB\BSON\MinKey`) pass; others are rejected.
- Preserves copy-on-write for strings and scalar arrays across clones.
- Preserves object identity, PHP `&` hard references, cycles, private/protected
  properties, `__serialize`/`__unserialize`/`__sleep`/`__wakeup` semantics,
  named closures (first-class callables), and enum values.
- Fuzz tests: 500-iteration seeded round-trip + 200-iteration malformed-input
  decoder test, both 32-bit safe.
- Compatible with PHP 8.2–8.5, NTS and ZTS, on x86_64 and i386 Linux,
  macOS, and Windows.
