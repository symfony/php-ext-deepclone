--TEST--
deepclone_from_array() rejects the internal classes that keep their state out of their properties, like deepclone_to_array() and deepclone_hydrate()
--EXTENSIONS--
deepclone
--FILE--
<?php

class UserIterator extends IteratorIterator { public $a; }
class UserIteratorWithWakeup extends IteratorIterator { public function __wakeup(): void {} }

function check(string $label, callable $fn): void
{
    try {
        $r = $fn();
        echo "$label: ", get_debug_type($r), "\n";
    } catch (Throwable $e) {
        echo "$label: ", $e::class, ': ', $e->getMessage(), "\n";
    }
}

function payload(string $class): array
{
    // What the polyfill produces for an object with no properties
    return ['classes' => $class, 'objectMeta' => 1, 'prepared' => 0];
}

check('to_array IteratorIterator', fn () => deepclone_to_array(new IteratorIterator(new ArrayIterator([1]))));
check('hydrate IteratorIterator', fn () => deepclone_hydrate('IteratorIterator'));
check('from_array IteratorIterator', fn () => deepclone_from_array(payload('IteratorIterator')));
check('from_array iteratoriterator', fn () => deepclone_from_array(payload('iteratoriterator')));
check('from_array AppendIterator', fn () => deepclone_from_array(payload('AppendIterator')));

// Objects holding closures are created as lazy ghosts on PHP 8.4+
$payload = payload('IteratorIterator') + ['properties' => ['stdClass' => ['f' => [[null, 'strlen']]]], 'resolve' => ['stdClass' => ['f' => [0]]]];
check('from_array lazy IteratorIterator', fn () => deepclone_from_array($payload, null, true));

// Classes of other extensions, when loaded: some crash when used, or even
// destroyed, without their constructor
foreach (['XMLWriter', 'XMLReader', 'ZipArchive', 'Redis', 'RedisCluster', 'Imagick', 'ImagickPixel', 'Relay\Table', 'AMQPConnection', 'APCUIterator'] as $class) {
    if (!class_exists($class) || method_exists($class, '__unserialize')) {
        continue;
    }
    $subclass = 'User'.strtr($class, '\\', '_');
    eval("class $subclass extends $class {}");
    foreach ([$class, $subclass] as $class) {
        try {
            deepclone_from_array(payload($class));
            echo "from_array $class: accepted\n";
        } catch (DeepClone\NotInstantiableException $e) {
            if ($e->getMessage() !== "Type \"$class\" is not instantiable.") {
                echo "from_array $class: ", $e->getMessage(), "\n";
            }
        }
    }
}

// User subclasses, unless they declare a serialization API, and the internal
// classes unserialize() creates all the same
check('to_array UserIterator', fn () => deepclone_to_array(new UserIterator(new ArrayIterator([1]))));
check('from_array UserIterator', fn () => deepclone_from_array(payload('UserIterator')));
check('hydrate UserIterator', fn () => deepclone_hydrate('UserIterator'));
check('from_array UserIteratorWithWakeup', fn () => deepclone_from_array(payload('UserIteratorWithWakeup')));
check('hydrate UserIteratorWithWakeup', fn () => deepclone_hydrate('UserIteratorWithWakeup'));
check('from_array MultipleIterator', fn () => deepclone_from_array(payload('MultipleIterator')));
check('round-trip SplMinHeap', fn () => deepclone_from_array(deepclone_to_array(new SplMinHeap())));
if (class_exists(DOMNodeList::class) && !deepclone_from_array(payload('DOMNodeList')) instanceof DOMNodeList) {
    echo "from_array DOMNodeList: rejected\n";
}
?>
--EXPECT--
to_array IteratorIterator: DeepClone\NotInstantiableException: Type "IteratorIterator" is not instantiable.
hydrate IteratorIterator: DeepClone\NotInstantiableException: Type "IteratorIterator" is not instantiable.
from_array IteratorIterator: DeepClone\NotInstantiableException: Type "IteratorIterator" is not instantiable.
from_array iteratoriterator: DeepClone\NotInstantiableException: Type "IteratorIterator" is not instantiable.
from_array AppendIterator: DeepClone\NotInstantiableException: Type "AppendIterator" is not instantiable.
from_array lazy IteratorIterator: DeepClone\NotInstantiableException: Type "IteratorIterator" is not instantiable.
to_array UserIterator: DeepClone\NotInstantiableException: Type "UserIterator" is not instantiable.
from_array UserIterator: DeepClone\NotInstantiableException: Type "UserIterator" is not instantiable.
hydrate UserIterator: DeepClone\NotInstantiableException: Type "UserIterator" is not instantiable.
from_array UserIteratorWithWakeup: UserIteratorWithWakeup
hydrate UserIteratorWithWakeup: UserIteratorWithWakeup
from_array MultipleIterator: MultipleIterator
round-trip SplMinHeap: SplMinHeap
