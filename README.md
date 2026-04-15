# deepclone

[![CI](https://github.com/symfony/php-ext-deepclone/actions/workflows/test.yml/badge.svg)](https://github.com/symfony/php-ext-deepclone/actions/workflows/test.yml)
[![License](https://img.shields.io/badge/license-MIT-blue.svg)](LICENSE)

A PHP extension that deep-clones any serializable PHP value while preserving
copy-on-write for strings and arrays — resulting in lower memory usage and
better performance than `unserialize(serialize())`.

It works by converting the value graph to a pure-array representation (only
scalars and nested arrays, no objects) and back. This array form is the wire
format used by Symfony's
[`VarExporter\DeepCloner`](https://symfony.com/doc/current/components/var_exporter.html),
making the extension a transparent drop-in accelerator.

## Use cases

**Repeated cloning of a prototype.** Calling `unserialize(serialize())` in a
loop allocates fresh copies of every string and array, blowing up memory.
This extension preserves PHP's copy-on-write: strings and scalar arrays are
shared between clones until they are actually modified.

```php
$payload = deepclone_to_array($prototype);
for ($i = 0; $i < 1000; $i++) {
    $clone = deepclone_from_array($payload);  // fast, COW-friendly
}
```

**OPcache-friendly cache format.** The pure-array payload is suitable for
`var_export()`. When cached in a `.php` file, OPcache maps it into shared
memory — making the "unserialize" step essentially free:

```php
// Write:
file_put_contents('cache.php', '<?php return ' . var_export(deepclone_to_array($graph), true) . ';');

// Read (OPcache serves this from SHM):
$clone = deepclone_from_array(require 'cache.php');
```

**Serialization to any format.** The array form can be passed to
`json_encode()`, MessagePack, igbinary, APCu, or any transport that handles
plain PHP arrays — without losing object identity, cycles, references, or
private property state.

```php
$payload = deepclone_to_array($graph);
$json = json_encode($payload);   // safe — no objects in the array
// ... send over the wire, store in a DB, etc.
$clone = deepclone_from_array(json_decode($json, true));
```

**Fast object instantiation and hydration.** Create objects and set their
properties — including private, protected, and readonly ones — without calling
their constructor, faster than Reflection:

```php
// Scoped array — keyed by declaring class
$user = deepclone_hydrate(User::class, [
    User::class => ['id' => 42, 'name' => 'Alice'],
    AbstractEntity::class => ['createdAt' => new \DateTimeImmutable()],
]);

// Flat bare-name array — ideal for hydrating from a flat row
// (e.g. a PDO result), no scope grouping needed
$user = deepclone_hydrate(User::class,
    ['id' => 42, 'name' => 'Alice', 'email' => 'alice@example.com'],
    DEEPCLONE_HYDRATE_MANGLED_VARS,
);

// Mangled keys (same format as (array) $obj cast)
$user = deepclone_hydrate(User::class,
    ['name' => 'Alice', "\0User\0email" => 'alice@example.com'],
    DEEPCLONE_HYDRATE_MANGLED_VARS,
);

// Hydrate an existing object
deepclone_hydrate($existingUser, ['User' => ['name' => 'Bob']]);
```

## API

```php
function deepclone_to_array(mixed $value, ?array $allowed_classes = null): array;
function deepclone_from_array(array $data, ?array $allowed_classes = null): mixed;
function deepclone_hydrate(object|string $object_or_class, array $vars = [], int $flags = 0): object;
```

`$allowed_classes` restricts which classes may be serialized or deserialized
(`null` = allow all, `[]` = allow none). Case-insensitive, matching
`unserialize()`'s `allowed_classes` option. Closures require `"Closure"` in
the list.

`deepclone_hydrate()` accepts either an object to hydrate in place or a class
name to instantiate without calling its constructor. By default, PHP `&`
references in `$vars` are dropped on write; pass `DEEPCLONE_HYDRATE_PRESERVE_REFS`
to keep them.

By default, `$vars` is keyed by declaring class name; each value is an array
of property names to values:

```php
$user = deepclone_hydrate(User::class, [
    User::class => ['id' => 42, 'name' => 'Alice'],
    AbstractEntity::class => ['createdAt' => new \DateTimeImmutable()],
]);
```

Pass `DEEPCLONE_HYDRATE_MANGLED_VARS` in `$flags` to interpret `$vars` as a
flat key array. Keys can be bare property names (auto-resolved to the
declaring class, the ideal shape for hydrating from a PDO row), or
mangled (`"\0ClassName\0prop"` for private, `"\0*\0prop"` for protected — the
same shape `(array) $object` produces). The two forms can be mixed:

```php
// Bare names — each is resolved to its declaring class automatically
$user = deepclone_hydrate(User::class,
    ['id' => 42, 'name' => 'Alice', 'email' => 'alice@example.com'],
    DEEPCLONE_HYDRATE_MANGLED_VARS,
);

// Mangled keys, typical (array) cast shape
$user = deepclone_hydrate(User::class,
    ['name' => 'Alice', "\0User\0email" => 'alice@example.com'],
    DEEPCLONE_HYDRATE_MANGLED_VARS,
);
```

Both the scoped shape and the flat-bare-name shape take a direct
`properties_info` lookup per key followed by a direct slot write — there's
no meaningful performance difference between the two, pick whichever
representation your caller already has on hand.

`$flags` selects the write semantics for declared-property assignments:

| Flag                                   | Semantics                                  |
|----------------------------------------|--------------------------------------------|
| `0` (default)                          | `ReflectionProperty::setRawValue` — bypass set hooks, type-check, respect readonly |
| `DEEPCLONE_HYDRATE_CALL_HOOKS`         | `ReflectionProperty::setValue` — invoke set hooks |
| `DEEPCLONE_HYDRATE_NO_LAZY_INIT`       | `ReflectionProperty::setRawValueWithoutLazyInitialization` — skip the lazy initializer; realize the object when the last lazy property is set |
| `DEEPCLONE_HYDRATE_MANGLED_VARS`       | interpret `$vars` as a flat mangled-key array (above) |
| `DEEPCLONE_HYDRATE_PRESERVE_REFS`      | preserve PHP `&` references from `$vars` onto the target property slots; by default, references are dropped (dereferenced) on write |

`DEEPCLONE_HYDRATE_CALL_HOOKS` and `DEEPCLONE_HYDRATE_NO_LAZY_INIT` are
mutually exclusive; `MANGLED_VARS` and `PRESERVE_REFS` compose with either.
`deepclone_from_array()` always uses the default setRawValue semantics,
mirroring `unserialize()`.

`PRESERVE_REFS` is off by default because preserving references requires a
per-call probe of the input array, which costs more than the typical DTO
hydration saves by using the ext over Reflection. Pass the flag when you
actually need a property slot to remain aliased to a caller-side variable or
to another property (e.g. when rehydrating a graph previously exported with
`deepclone_to_array()` that contained `&` references).

### Forgiving payload handling

`deepclone_hydrate()` applies three coercions before writing each
declared property, so common rehydration patterns don't trip on
strict-type errors. They run under every mode unless noted:

- **Readonly idempotent skip** — when the readonly slot already holds an
  identical value (`===`), the write is silently skipped. Avoids
  `Error: Cannot modify readonly property` on no-op rehydration.
  Different values still raise the engine's normal error.
- **`null` → `unset()` for non-nullable typed properties** — writing
  `null` into a non-nullable typed slot stores the uninitialized state
  (so `ReflectionProperty::isInitialized()` returns `false` and reads
  raise the standard "must not be accessed before initialization"
  error) instead of throwing `TypeError`. This restores a state
  otherwise unreachable through hydration. Nullable / `mixed` types
  keep their existing semantics. Hooked properties never trigger this
  rule (no backing slot to "unset" semantically; a set hook may handle
  `null` itself).
- **Scalar → backed-enum cast** — when the property is typed with a
  single (possibly nullable) backed enum and the payload value is a
  scalar matching the enum's backing type (`int` ↔ int-backed,
  `string` ↔ string-backed), the value is cast to the corresponding
  case. Unknown backing values raise the standard `ValueError` ("X is
  not a valid backing value for enum Y"), matching `Enum::from()`.
  Union/intersection types on the property itself are left untouched.
  The decision rests on the property type only — hook presence and
  `DEEPCLONE_HYDRATE_CALL_HOOKS` mode don't change it. Set hooks on
  enum-typed properties accordingly receive the enum case, not the
  raw scalar.

The special `"\0"` key sets the internal state of SPL classes. In scoped
mode it goes inside a scope entry; in `MANGLED_VARS` mode it is a flat key:

```php
// Scoped mode:
$ao = deepclone_hydrate('ArrayObject', ['ArrayObject' => ["\0" => [['x' => 1]]]]);

// MANGLED_VARS mode:
$ao = deepclone_hydrate('ArrayObject',
    ["\0" => [['x' => 1], ArrayObject::ARRAY_AS_PROPS]],
    DEEPCLONE_HYDRATE_MANGLED_VARS,
);

// SplObjectStorage: "\0" => [$obj1, $info1, $obj2, $info2, ...]
$s = deepclone_hydrate('SplObjectStorage',
    ["\0" => [$obj, 'metadata']],
    DEEPCLONE_HYDRATE_MANGLED_VARS,
);
```

## What it preserves

- Object identity (shared references stay shared)
- PHP `&` hard references
- Cycles in the object graph
- Private/protected properties across inheritance
- `__serialize` / `__unserialize` / `__sleep` / `__wakeup` semantics
- Named closures (first-class callables like `strlen(...)`)
- Enum values
- Copy-on-write for strings and scalar arrays

## Error handling

| Exception                            | Thrown by                                  | When                                                     |
| ------------------------------------ | ------------------------------------------ | -------------------------------------------------------- |
| `DeepClone\NotInstantiableException` | `deepclone_to_array`, `deepclone_hydrate`  | Resource, anonymous class, `Reflection*`, internal class without serialization support |
| `DeepClone\ClassNotFoundException`   | `deepclone_from_array`, `deepclone_hydrate`| Payload/class name references a class that doesn't exist |
| `ValueError`                         | all three                                  | Malformed input, or class not in `$allowed_classes`      |

Both exception classes extend `\InvalidArgumentException`.

## Requirements

- PHP 8.2+ (NTS or ZTS, 64-bit and 32-bit)

## Installation

### With PIE (recommended)

```bash
pie install symfony/deepclone
```

Then enable in `php.ini`:

```ini
extension=deepclone
```

### Manual build

```bash
git clone https://github.com/symfony/php-ext-deepclone.git
cd php-ext-deepclone
phpize && ./configure --enable-deepclone && make && make test
sudo make install
```

## With Symfony

`symfony/var-exporter` and `symfony/polyfill-deepclone` provide the same
`deepclone_to_array()`, `deepclone_from_array()`, and `deepclone_hydrate()`
functions in pure PHP. When this extension is loaded it replaces the polyfill
transparently — no code change needed.

Symfony's `Hydrator::hydrate()` and `Instantiator::instantiate()` delegate
directly to `deepclone_hydrate()`, making them thin one-liner wrappers.

## License

Released under the [MIT license](LICENSE).
