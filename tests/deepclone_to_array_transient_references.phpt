--TEST--
deepclone_to_array() tells apart the references that __serialize() creates and releases
--EXTENSIONS--
deepclone
--FILE--
<?php

class State
{
    public function __construct(public $v) {}
    public function __serialize(): array { $x = $this->v; return ['a' => &$x, 'b' => &$x]; }
    public function __unserialize(array $data): void { $this->v = $data['a']; }
}

$clone = deepclone_from_array(deepclone_to_array([new State('first'), new State('second'), new State('third')]));
var_dump($clone[0]->v, $clone[1]->v, $clone[2]->v);

?>
--EXPECT--
string(5) "first"
string(6) "second"
string(5) "third"
