--TEST--
deepclone_from_array() and deepclone_hydrate() reject the classes unserialize() can't create with NotInstantiableException
--EXTENSIONS--
deepclone
--FILE--
<?php

abstract class AbstractThing { public $f; }
interface Thing {}
trait ThingTrait {}
enum ThingEnum { case A; }
class UserIterator extends IteratorIterator { public $a; }

function check(string $label, callable $fn): void
{
    try {
        $r = $fn();
        echo "$label: ", get_debug_type($r), "\n";
    } catch (Throwable $e) {
        echo "$label: ", $e::class, ': ', $e->getMessage(), "\n";
    }
}

// Named in lower case: messages use the declared name
foreach (['AbstractThing', 'Thing', 'ThingTrait', 'ThingEnum'] as $class) {
    check("from_array $class", fn () => deepclone_from_array(['classes' => strtolower($class), 'objectMeta' => 1, 'prepared' => 0]));
    check("hydrate $class", fn () => deepclone_hydrate(strtolower($class)));
}

// Objects holding closures are created as lazy ghosts on PHP 8.4+
$payload = ['classes' => 'AbstractThing', 'objectMeta' => 1, 'prepared' => 0, 'properties' => ['stdClass' => ['f' => [[null, 'strlen']]]], 'resolve' => ['stdClass' => ['f' => [0]]]];
check('from_array lazy AbstractThing', fn () => deepclone_from_array($payload, null, true));

// unserialize() creates the classes whose state serialize() loses, and so
// does deepclone_from_array(), while deepclone_to_array() rejects them
foreach (['IteratorIterator', 'LimitIterator', 'UserIterator'] as $class) {
    check("unserialize $class", fn () => unserialize('O:'.strlen($class).':"'.$class.'":0:{}'));
    check("from_array $class", fn () => deepclone_from_array(['classes' => $class, 'objectMeta' => 1, 'prepared' => 0]));
}
check('to_array IteratorIterator', fn () => deepclone_to_array(new IteratorIterator(new ArrayIterator([1]))));
check('hydrate IteratorIterator', fn () => deepclone_hydrate('IteratorIterator'));
check('hydrate UserIterator', fn () => deepclone_hydrate('UserIterator', ['a' => 1]));
?>
--EXPECT--
from_array AbstractThing: DeepClone\NotInstantiableException: Type "AbstractThing" is not instantiable.
hydrate AbstractThing: DeepClone\NotInstantiableException: Type "AbstractThing" is not instantiable.
from_array Thing: DeepClone\NotInstantiableException: Type "Thing" is not instantiable.
hydrate Thing: DeepClone\NotInstantiableException: Type "Thing" is not instantiable.
from_array ThingTrait: DeepClone\NotInstantiableException: Type "ThingTrait" is not instantiable.
hydrate ThingTrait: DeepClone\NotInstantiableException: Type "ThingTrait" is not instantiable.
from_array ThingEnum: DeepClone\NotInstantiableException: Type "ThingEnum" is not instantiable.
hydrate ThingEnum: DeepClone\NotInstantiableException: Type "ThingEnum" is not instantiable.
from_array lazy AbstractThing: DeepClone\NotInstantiableException: Type "AbstractThing" is not instantiable.
unserialize IteratorIterator: IteratorIterator
from_array IteratorIterator: IteratorIterator
unserialize LimitIterator: LimitIterator
from_array LimitIterator: LimitIterator
unserialize UserIterator: UserIterator
from_array UserIterator: UserIterator
to_array IteratorIterator: DeepClone\NotInstantiableException: Type "IteratorIterator" is not instantiable.
hydrate IteratorIterator: DeepClone\NotInstantiableException: Type "IteratorIterator" is not instantiable.
hydrate UserIterator: UserIterator
