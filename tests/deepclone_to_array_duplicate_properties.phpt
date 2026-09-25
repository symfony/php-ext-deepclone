--TEST--
deepclone_to_array() keeps the last of the keys that resolve to the same property, like unserialize()
--EXTENSIONS--
deepclone
--FILE--
<?php

class DupBase
{
    private $secret = 'default';

    public function setSecret($v): void { $this->secret = $v; }
}

// unserialize() writes a dynamic property named like a private property of a
// parent class to that private property
#[AllowDynamicProperties]
class DupDynamic extends DupBase
{
}

// A dynamic property goes to the private property of the closest parent class
class DupGrandParent
{
    private $x = 'default';

    public function setGrandParent($v): void { $this->x = $v; }
}

class DupParent extends DupGrandParent
{
    private $x = 'default';

    public function setParent($v): void { $this->x = $v; }
}

#[AllowDynamicProperties]
class DupTwoLevels extends DupParent
{
}

// Without __unserialize(), the bare and the mangled name of a property both
// set it
class DupSerialize
{
    private $b = 'default';
    public $data;

    public function __serialize(): array { return $this->data; }
}

function check(object $o): void
{
    $data = deepclone_to_array($o);
    echo json_encode($data['properties']), "\n";
    var_dump((array) deepclone_from_array($data) == (array) unserialize(serialize($o)));
}

$o = new DupDynamic();
$o->setSecret('parent');
$o->secret = 'dynamic';
check($o);

$o = new DupDynamic();
$o->setSecret('parent');
$o->secret = 'default';
check($o);

$o = new DupTwoLevels();
$o->setGrandParent('grandparent');
$o->setParent('parent');
$o->x = 'dynamic';
check($o);

$o = new DupSerialize();
$o->data = ['b' => 'bare', "\0DupSerialize\0b" => 'mangled'];
check($o);

$o = new DupSerialize();
$o->data = ["\0DupSerialize\0b" => 'mangled', 'b' => 'bare'];
check($o);
?>
--EXPECT--
{"DupBase":{"secret":["dynamic"]}}
bool(true)
{"DupBase":{"secret":["default"]}}
bool(true)
{"DupGrandParent":{"x":["grandparent"]},"DupParent":{"x":["dynamic"]}}
bool(true)
{"DupSerialize":{"b":["mangled"]}}
bool(true)
{"DupSerialize":{"b":["bare"]}}
bool(true)
