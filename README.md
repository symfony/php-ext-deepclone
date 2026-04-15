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
// Set private properties via scoped array (fastest path)
$user = deepclone_hydrate(User::class, [
    User::class => ['id' => 42, 'name' => 'Alice'],
    AbstractEntity::class => ['createdAt' => new \DateTimeImmutable()],
]);

// Instantiate from class name with mangled keys (same format as (array) cast)
$user = deepclone_hydrate(User::class, [], ['name' => 'Alice', 'email' => 'alice@example.com']);

// Hydrate an existing object
deepclone_hydrate($existingUser, ['User' => ['name' => 'Bob']]);
```

## API

```php
function deepclone_to_array(mixed $value, ?array $allowed_classes = null): array;
function deepclone_from_array(array $data, ?array $allowed_classes = null): mixed;
function deepclone_hydrate(object|string $object_or_class, array $scoped_vars = [], array $mangled_vars = [], int $flags = 0): object;
```

`$allowed_classes` restricts which classes may be serialized or deserialized
(`null` = allow all, `[]` = allow none). Case-insensitive, matching
`unserialize()`'s `allowed_classes` option. Closures require `"Closure"` in
the list.

`deepclone_hydrate()` accepts either an object to hydrate in place or a class
name to instantiate without calling its constructor. Both `$scoped_vars` and
`$mangled_vars` can be used together; `$mangled_vars` values are resolved
and merged into `$scoped_vars` before hydration. PHP `&` references are
preserved in both.

`$scoped_vars` is the **fastest path** — it writes directly to property
slots with no key parsing, making it ideal for hot loops. It is keyed by
declaring class name, each value being an array of property names to values.

`$mangled_vars` accepts the same mangled key format as `(array) $object`
(`"\0ClassName\0prop"` for private, `"\0*\0prop"` for protected, bare name
for public/dynamic). It resolves each key to the correct scope automatically,
which is handy when round-tripping with `(array)` casts.

`$flags` selects the write semantics for declared-property assignments:

| Flag                                   | Semantics                                  |
|----------------------------------------|--------------------------------------------|
| `0` (default)                          | `ReflectionProperty::setRawValue` — bypass set hooks, type-check, respect readonly |
| `DEEPCLONE_HYDRATE_CALL_HOOKS`         | `ReflectionProperty::setValue` — invoke set hooks |

`deepclone_from_array()` always uses the default setRawValue semantics,
mirroring `unserialize()`.

For lazy objects, writes go through the engine's standard path, so the
lazy initializer fires on first access — same as `ReflectionProperty::setValue`
or `setRawValue`. Suppressing the initializer requires
`ReflectionProperty::setRawValueWithoutLazyInitialization`, which depends
on engine helpers that aren't `ZEND_API`-exported and therefore aren't
reachable from a shared extension.

The special `"\0"` key sets the internal state of SPL classes. In
`$scoped_vars` it goes inside a scope entry; in `$mangled_vars` it is a
flat key:

```php
// Via $scoped_vars:
$ao = deepclone_hydrate('ArrayObject', ['ArrayObject' => ["\0" => [['x' => 1]]]]);

// Via $mangled_vars:
$ao = deepclone_hydrate('ArrayObject', [], ["\0" => [['x' => 1], ArrayObject::ARRAY_AS_PROPS]]);

// SplObjectStorage: "\0" => [$obj1, $info1, $obj2, $info2, ...]
$s = deepclone_hydrate('SplObjectStorage', [], ["\0" => [$obj, 'metadata']]);
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
