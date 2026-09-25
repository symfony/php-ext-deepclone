# deepclone

[![CI](https://github.com/symfony/php-ext-deepclone/actions/workflows/test.yml/badge.svg)](https://github.com/symfony/php-ext-deepclone/actions/workflows/test.yml)
[![License](https://img.shields.io/badge/license-MIT-blue.svg)](LICENSE)

This extension deep-clones any serializable PHP value, faster and with less
memory than `unserialize(serialize())`: strings and arrays keep their
copy-on-write semantics.

It turns the value into a payload made of scalars and nested arrays only, and
back. That's the format of Symfony's
[`DeepCloner`](https://symfony.com/doc/current/components/var_exporter.html),
which uses the extension when it's loaded and
[its polyfill](https://github.com/symfony/polyfill/tree/1.x/src/DeepClone)
otherwise.

## Use cases

Cloning a prototype many times, with strings and arrays shared until they're
modified:

```php
$payload = deepclone_to_array($prototype);

for ($i = 0; $i < 1000; ++$i) {
    $clone = deepclone_from_array($payload);
}
```

Caching with OPcache: dump the payload into a `.php` file with `var_export()`,
and loading it becomes essentially free since OPcache serves it from shared
memory:

```php
file_put_contents('cache.php', '<?php return '.var_export(deepclone_to_array($graph), true).';');

$clone = deepclone_from_array(require 'cache.php');
```

Sending a graph through JSON, MessagePack, APCu or any transport that handles
plain arrays, while keeping object identities, cycles, references and private
state:

```php
$json = json_encode(deepclone_to_array($graph));

$clone = deepclone_from_array(json_decode($json, true));
```

Creating and hydrating objects without calling their constructor, private,
protected and readonly properties included, faster than Reflection:

```php
$user = deepclone_hydrate(User::class, ['id' => 42, 'name' => 'Alice']);

deepclone_hydrate($user, ['name' => 'Bob']);
```

## API

```php
function deepclone_to_array(mixed $value, ?array $allowed_classes = null, bool $allow_named_closures = false): array;
function deepclone_from_array(array $data, ?array $allowed_classes = null, bool $allow_named_closures = false): mixed;
function deepclone_hydrate(object|string $object_or_class, array $vars = [], int $flags = 0): object;
```

`$allowed_classes` works like the `allowed_classes` option of `unserialize()`,
with `null` allowing any class. Names are case-insensitive.

`$allow_named_closures` allows encoding closures over named callables by name,
eg `strlen(...)` or `$obj->method(...)`. Both ends must enable it: resolving
such a payload can create a closure over any function or method of that name,
`system()` included, so do this only between ends that trust each other.
Closures declared in constant expressions don't need it, eg
`#[When(self::isStrict(...))]`: they're encoded as a reference to their
declaration site, which resolves only to what the class itself declares.

## Lazy hydration

Resolving closures is where hydration time goes. That's why, on PHP 8.4+,
`deepclone_from_array()` creates the objects whose properties or
`__unserialize()` state hold closures as
[lazy ghosts](https://www.php.net/manual/en/language.oop5.lazy-objects.php),
hydrated when first used. All objects exist when the call returns, with their
identities and references. The other objects are hydrated right away, and so
are instances of internal classes or of classes that declare no properties:
graphs without closures pay nothing for this.

The rules of lazy objects apply, with a few consequences:

- malformed payloads and disallowed classes are still rejected right away,
  but errors that depend on values, like a class or enum case that doesn't
  exist anymore, are thrown on first use, and again on each retry;
- `__wakeup()` and `__unserialize()` of such objects are called when they're
  initialized;
- writing through a shared `&` reference checks the types of the objects
  already hydrated only - the others throw when first used if the value
  doesn't fit them;
- the payload stays in memory until the last lazy object is initialized or
  destructed.

On closure-heavy graphs of 20k objects, this makes cloning 4-6x faster and
the copy 2-3x lighter while it's not used. Using all its objects costs about
the same in total.

## Hydration

`deepclone_hydrate()` hydrates an object, or instantiates a class without
calling its constructor. `$vars` uses the keys of `(array) $object`:

| Key                | Property                                                            |
|--------------------|---------------------------------------------------------------------|
| `"name"`           | public, protected, or private declared by the object's class        |
| `"\0*\0name"`      | protected                                                           |
| `"\0Parent\0name"` | private declared by `Parent`, the object's class or a parent of it |

`$flags` selects how properties are written:

| Flag                              | Writes                                                            |
|-----------------------------------|-------------------------------------------------------------------|
| `0`                               | like `ReflectionProperty::setRawValue()`, without set hooks       |
| `DEEPCLONE_HYDRATE_CALL_HOOKS`    | like `ReflectionProperty::setValue()`, running set hooks          |
| `DEEPCLONE_HYDRATE_NO_LAZY_INIT`  | like `ReflectionProperty::setRawValueWithoutLazyInitialization()` |
| `DEEPCLONE_HYDRATE_PRESERVE_REFS` | keeping the `&` references of `$vars`, where PHP allows them      |

`CALL_HOOKS` and `NO_LAZY_INIT` are mutually exclusive. `PRESERVE_REFS` isn't
the default because finding references costs more than hydrating most
objects. `deepclone_from_array()` writes like the default, as `unserialize()`
does.

`deepclone_hydrate()` also forgives what rehydrating often trips on: `null`
leaves a non-nullable typed property uninitialized instead of throwing, a
scalar written to a property typed with a backed enum becomes the matching
case, and writing the value a readonly property already holds does nothing.

For `ArrayObject`, `SplObjectStorage` and the other classes that keep their
state internally, call `__unserialize()` after instantiating them, or use
`deepclone_from_array()`, which does it for you:

```php
$ao = deepclone_hydrate(ArrayObject::class);
$ao->__unserialize([ArrayObject::ARRAY_AS_PROPS, ['x' => 1], []]);
```

## What it preserves

- object identities, cycles and `&` references, between array elements and
  properties alike;
- private and protected properties across inheritance;
- the semantics of `__serialize()`, `__unserialize()`, `__sleep()` and
  `__wakeup()`;
- enums, and closures as described above;
- the state of lazy objects: `deepclone_to_array()` initializes them first,
  like `clone` does and regardless of `SKIP_INITIALIZATION_ON_SERIALIZE`, and a
  lazy proxy comes back as an instance of its own class, as with
  `unserialize(serialize())`;
- copy-on-write for strings and arrays of scalars.

## Errors

`deepclone_to_array()` and `deepclone_hydrate()` throw
`DeepClone\NotInstantiableException` for resources, `Reflection*` and
internal classes that can't be serialized. All three functions throw it for
classes that refuse serialization, subclasses of internal ones included,
whatever methods they declare, like `serialize()` does. Anonymous classes
refuse it too, but round-trip when they declare `__wakeup()` or
`__unserialize()`, like throwables do.
`deepclone_from_array()` and `deepclone_hydrate()` throw it for abstract
classes, interfaces, traits and enums, and throw
`DeepClone\ClassNotFoundException` for classes that don't exist. Both extend
`InvalidArgumentException`. Malformed input and classes missing from
`$allowed_classes` throw `ValueError`.

## Installation

The extension requires PHP 8.2+, NTS or ZTS, 32 or 64-bit. Install it with
[PIE](https://github.com/php/pie), then add `extension=deepclone` to your
`php.ini` if PIE didn't:

```bash
pie install symfony/deepclone
```

Or build it:

```bash
git clone https://github.com/symfony/php-ext-deepclone.git
cd php-ext-deepclone
phpize && ./configure --enable-deepclone && make && make test
sudo make install
```

## With Symfony

`symfony/polyfill-deepclone` provides the same functions in pure PHP, and the
extension takes over when it's loaded, with no code change. VarExporter's
`DeepCloner` builds on them, and its `Hydrator::hydrate()` and
`Instantiator::instantiate()` are one-line wrappers around
`deepclone_hydrate()`.

## License

Released under the [MIT license](LICENSE).
