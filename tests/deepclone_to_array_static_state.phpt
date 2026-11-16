--TEST--
deepclone_to_array() shares the state that __serialize() returns when it holds scalars only
--EXTENSIONS--
deepclone
--FILE--
<?php

class Rows
{
    public $rows = [];

    public function __serialize(): array { return $this->rows; }
    public function __unserialize(array $data): void { $this->rows = $data; }
}

// A state made of scalars is shared with the payload, like the static arrays of properties
$o = new Rows();
$o->rows = range(1, 100000);

$before = memory_get_usage();
$payload = deepclone_to_array($o);
var_dump(memory_get_usage() - $before < 100000);

// and stays what it was when the object changes
$o->rows[0] = 'changed';
var_dump($payload['states'][1][1][0]);

$clone = deepclone_from_array($payload);
var_dump(count($clone->rows), $clone->rows[0], $clone->rows[99999]);

// An empty state, a literal one and one that holds an object
class State
{
    public $state;

    public function __serialize(): array { return $this->state; }
    public function __unserialize(array $data): void { $this->state = $data; }
}

foreach ([[], ['a' => 1], ['o' => new stdClass(), 'n' => [1]]] as $state) {
    $o = new State();
    $o->state = $state;
    var_dump(deepclone_from_array(deepclone_to_array($o))->state == $state);
}

?>
--EXPECT--
bool(true)
int(1)
int(100000)
int(1)
int(100000)
bool(true)
bool(true)
bool(true)
