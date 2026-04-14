# Changelog

All notable changes to this extension will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

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

### Changed

- All function parameters now use snake_case to follow PHP conventions:
  `$allowed_classes`, `$object_or_class`, `$scoped_vars`, `$mangled_vars`.
- `deepclone_from_array()` now writes declared properties via direct
  `OBJ_PROP` slot access (same fast path as `deepclone_hydrate`), including
  correct `zend_reference` type-source tracking for typed properties. On a
  50-node graph this is ~25% faster and also covers a latent assertion on
  references flowing through typed user-class properties.
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
