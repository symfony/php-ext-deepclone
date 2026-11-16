--TEST--
deepclone_to_array() leaves the cycles of objects that __serialize() creates to the cycle collector, also when it runs during the call
--EXTENSIONS--
deepclone
--FILE--
<?php

class Node
{
    public static int $alive = 0;
    public $peer;

    public function __construct() { ++self::$alive; }
    public function __destruct() { --self::$alive; }
}

class Holder
{
    public function __serialize(): array
    {
        $a = new Node();
        $a->peer = new Node();
        $a->peer->peer = $a;

        return ['node' => $a];
    }

    public function __unserialize(array $data): void {}
}

class Collector
{
    public function __serialize(): array { gc_collect_cycles(); return []; }
    public function __unserialize(array $data): void {}
}

$payload = deepclone_to_array([new Holder(), new Collector(), new Holder()]);
unset($payload);
gc_collect_cycles();
var_dump(Node::$alive);

?>
--EXPECT--
int(0)
