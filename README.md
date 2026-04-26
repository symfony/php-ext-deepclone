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
// Flat bare-name array — ideal for hydrating from a flat row
// (e.g. a PDO result).
$user = deepclone_hydrate(User::class, [
    'id' => 42,
    'name' => 'Alice',
    'email' => 'alice@example.com',
]);

// Mangled keys for parent-declared private properties — same format as
// (array) $obj cast produces.
$user = deepclone_hydrate(User::class, [
    'name' => 'Alice',
    "\0AbstractEntity\0createdAt" => new \DateTimeImmutable(),
]);

// Hydrate an existing object
deepclone_hydrate($existingUser, ['name' => 'Bob']);
```

## API

```php
function deepclone_to_array(mixed $value, ?array $allowed_classes = null): array;
function deepclone_from_array(array $data, ?array $allowed_classes = null): mixed;
function deepclone_hydrate(object|string $object_or_class, array $vars = [], int $flags = 0): object;
```

`$allowed_classes` restricts which classes may be serialized or deserialized
(`null` = allow all, `[]` = allow none). Case-insensitive, matching
`unserialize()`'s `allowed_classes` option.

`deepclone_hydrate()` accepts either an object to hydrate in place or a class
name to instantiate without calling its constructor. By default, PHP `&`
references in `$vars` are dropped on write; pass `DEEPCLONE_HYDRATE_PRESERVE_REFS`
to keep them.

`$vars` is a flat array keyed by property name — the exact shape
`(array) $obj` produces:

| key shape                   | target                                                    |
|-----------------------------|-----------------------------------------------------------|
| `"propName"`                | public, protected (any declaring class), or private declared on the object's own class |
| `"\0*\0propName"`           | protected (the declaring class is resolved via the object) |
| `"\0ClassName\0propName"`   | private declared on `ClassName` — must be the object's own class or a parent |

Each key triggers one `properties_info` hash lookup followed by a direct
slot write.

```php
$user = deepclone_hydrate(User::class, [
    'id' => 42,                             // bare — public or own-private
    'name' => 'Alice',
    "\0*\0createdAt" => new \DateTimeImmutable(),    // protected
    "\0AbstractEntity\0metadata" => [...],           // parent-private
]);
```

Bare names are enough for every public, protected, or most-derived-private
property. Parent-declared private properties need the explicit
`"\0ClassName\0prop"` mangled form (the engine keys them that way in the
child's `properties_info`).

`$flags` selects the write semantics for declared-property assignments:

| Flag                                   | Semantics                                  |
|----------------------------------------|--------------------------------------------|
| `0` (default)                          | `ReflectionProperty::setRawValue` — bypass set hooks, type-check, respect readonly |
| `DEEPCLONE_HYDRATE_CALL_HOOKS`         | `ReflectionProperty::setValue` — invoke set hooks |
| `DEEPCLONE_HYDRATE_NO_LAZY_INIT`       | `ReflectionProperty::setRawValueWithoutLazyInitialization` — skip the lazy initializer; realize the object when the last lazy property is set |
| `DEEPCLONE_HYDRATE_PRESERVE_REFS`      | preserve PHP `&` references from `$vars` onto the target property slots; by default, references are dropped (dereferenced) on write |

`DEEPCLONE_HYDRATE_CALL_HOOKS` and `DEEPCLONE_HYDRATE_NO_LAZY_INIT` are
mutually exclusive; `PRESERVE_REFS` composes with either.
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

SPL classes that hold internal state (`ArrayObject`, `ArrayIterator`,
`SplObjectStorage`, …) have shipped `__serialize` / `__unserialize` since
PHP 7.4. To populate them, instantiate with `deepclone_hydrate()` and call
`__unserialize()` with the array shape the class documents — or just use
`deepclone_from_array()`, which routes through `__unserialize` natively.

```php
$ao = deepclone_hydrate('ArrayObject');
$ao->__unserialize([ArrayObject::ARRAY_AS_PROPS, ['x' => 1, 'y' => 2], []]);

$s = deepclone_hydrate('SplObjectStorage');
$s->__unserialize([[$obj1, 'info1', $obj2, 'info2'], []]);
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
