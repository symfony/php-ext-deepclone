--TEST--
deepclone_from_array() lazy hydration: malformed markers of deferred slots fail eagerly, like in eager slots
--EXTENSIONS--
deepclone
--SKIPIF--
<?php
if (PHP_VERSION_ID < 80400) {
    die('skip native lazy objects require PHP 8.4+');
}
?>
--FILE--
<?php

class Node { public mixed $v = null; public mixed $w = null; }
class Unser { public mixed $v = null; public function __unserialize(array $data): void {} }

function check(string $class, mixed $value, mixed $marker): void
{
    $messages = [];
    // A stdClass object is never a lazy ghost, a Node holding a closure is
    foreach (['stdClass', $class] as $c) {
        try {
            deepclone_from_array([
                'classes' => $c,
                'objectMeta' => 1,
                'prepared' => 0,
                'properties' => ['stdClass' => ['v' => [[null, 'strlen']], 'w' => [$value]]],
                'resolve' => ['stdClass' => ['v' => [0], 'w' => [$marker]]],
                'refs' => [1 => 'r'],
            ], null, true);
            $messages[] = 'returned';
        } catch (ValueError $e) {
            $messages[] = $e->getMessage();
        }
    }
    echo $messages[0] === $messages[1] ? $messages[0] : implode(' <> ', $messages), "\n";
}

check('Node', 5, true);
check('Node', 'x', true);
check('Node', -3, true);
check('Node', PHP_INT_MIN, true);
check('Node', -3, false);
check('Node', 2, false);
check('Node', 'x', false);
check('Node', 1, 'e');
check('Node', 'Foo', 'e');
check('Node', 'x', [true]);
check('Node', [7], [true]);
check('Node', [], [false]);
check('Node', [0 => 1], [1 => true]);
check('Node', [7, 'strlen'], 0);
check('Node', [1.5, 'strlen'], 0);
check('Node', ['Node', '', 0, 0], 1);
check('Node', ['Node', '', 0, -1, 1], 1);

// What depends on what the payload references fails on first access
$node = deepclone_from_array([
    'classes' => 'Node',
    'objectMeta' => 1,
    'prepared' => 0,
    'properties' => ['stdClass' => ['v' => [[null, 'strlen']], 'w' => [[-1, 'NoSuchEnum::Case']]]],
    'resolve' => ['stdClass' => ['v' => [0], 'w' => [[true, 'e']]]],
    'refs' => [1 => 'r'],
], null, true);
var_dump((new ReflectionClass(Node::class))->isUninitializedLazyObject($node));
try {
    $node->w;
} catch (ValueError $e) {
    echo $e->getMessage(), "\n";
}

// The state of an __unserialize() ghost is checked the same way
try {
    deepclone_from_array([
        'classes' => 'Unser',
        'objectMeta' => [[0, -1]],
        'prepared' => 0,
        'states' => [[0, ['c' => [null, 'strlen'], 'o' => 5], ['c' => 0, 'o' => true]]],
    ], null, true);
    echo "returned\n";
} catch (ValueError $e) {
    echo $e->getMessage(), "\n";
}

?>
--EXPECT--
deepclone_from_array(): malformed payload, unknown object id 5
deepclone_from_array(): malformed payload, object reference value must be of type int, string given
deepclone_from_array(): malformed payload, unknown ref id 3
deepclone_from_array(): malformed payload, ref id out of range
deepclone_from_array(): malformed payload, unknown ref id 3
deepclone_from_array(): malformed payload, ref id out of range
deepclone_from_array(): malformed payload, hard-ref value must be of type int, string given
deepclone_from_array(): malformed payload, enum value must be of type string, int given
deepclone_from_array(): malformed payload, enum value must match "Class::Case"
deepclone_from_array(): malformed payload, array-mask value must be of type array, string given
deepclone_from_array(): malformed payload, unknown object id 7
deepclone_from_array(): malformed payload, hard-ref slot must be of type int, null given
deepclone_from_array(): malformed payload, object reference value must be of type int, null given
deepclone_from_array(): malformed payload, named-closure references unknown id 7
deepclone_from_array(): malformed payload, named-closure callable must be [obj_or_class_or_null, string]
deepclone_from_array(): malformed payload, const-expr-closure value must have 5 elements
deepclone_from_array(): malformed payload, const-expr-closure references unknown closure index -1
bool(true)
deepclone_from_array(): malformed payload, enum class "NoSuchEnum::Case" not found
deepclone_from_array(): malformed payload, unknown object id 5
