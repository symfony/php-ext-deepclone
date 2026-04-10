# Changelog

All notable changes to this extension will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

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
