--TEST--
ReflectionAttribute finds the callables that attributes declare, in any graph of objects
--EXTENSIONS--
deepclone
--SKIPIF--
<?php
if (PHP_VERSION_ID < 80500) die('skip closures in constant expressions require PHP 8.5+');
?>
--FILE--
<?php

class Validators
{
    public static function check() { return 'checked'; }
    public static function other() { return 'other'; }
}

enum Level
{
    case Low;
    case High;
}

class Node
{
    public array $children = [];

    public function __construct(public $value = null, public ?Node $next = null, int $ring = 0)
    {
        // Nodes that reference this one back, through arrays
        $node = $this;
        for ($i = 0; $i < $ring; ++$i) {
            $node = $node->children[] = new Node();
            $node->children[] = $this;
        }
    }
}

#[Attribute(Attribute::TARGET_ALL)]
class Chain
{
    public function __construct(public $head = null, public $level = null, public array $options = [])
    {
    }
}

// A callable under a chain of objects, next to an enum case and to literal arrays
#[Chain(new Node(1, new Node(2, new Node(3, new Node(4, new Node(5, new Node(6, new Node(7, new Node(8, new Node(9, new Node(Validators::check(...))))))))))), Level::High, ['a' => [1, 2], 'b' => 'c'])]
class Deep
{
}

// A callable in a graph of objects that reference each other
#[Chain(new Node(Validators::other(...), null, 12), Level::Low)]
class Ring
{
}

foreach ([Deep::class, Ring::class] as $class) {
    $attribute = (new ReflectionClass($class))->getAttributes()[0];
    $instance = $attribute->newInstance();
    var_dump($instance->level, count($attribute->getArguments()));

    for ($node = $instance->head; $node->next; $node = $node->next);
    $payload = deepclone_to_array($node->value);
    echo json_encode($payload['prepared']), ' ', deepclone_from_array($payload)(), "\n";
}

?>
--EXPECT--
enum(Level::High)
int(3)
["Deep","",0,0,5] checked
enum(Level::Low)
int(2)
["Ring","",0,0,6] other
