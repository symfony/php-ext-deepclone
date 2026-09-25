--TEST--
deepclone_to_array() keeps a property named like a private property of a parent class in its own scope
--EXTENSIONS--
deepclone
--FILE--
<?php

class ShadowBase
{
    private $name = 'default';

    public function setBase($v): void { $this->name = $v; }

    public function getBase() { return $this->name; }
}

class ShadowPublic extends ShadowBase
{
    public $name = 'default';

    public function set($v): void { $this->name = $v; }

    public function get() { return $this->name; }
}

class ShadowProtected extends ShadowBase
{
    protected $name = 'default';

    public function set($v): void { $this->name = $v; }

    public function get() { return $this->name; }
}

foreach ([new ShadowPublic(), new ShadowProtected()] as $o) {
    $o->setBase('parent');
    $o->set('child');

    echo json_encode(deepclone_to_array($o)['properties']), "\n";
    // Building the property table moves the export to another code path
    foreach ($o as $v) {
    }
    $data = deepclone_to_array($o);
    echo json_encode($data['properties']), "\n";

    $clone = deepclone_from_array($data);
    var_dump($clone->getBase(), $clone->get());
}
?>
--EXPECT--
{"ShadowBase":{"name":["parent"]},"stdClass":{"name":["child"]}}
{"ShadowBase":{"name":["parent"]},"stdClass":{"name":["child"]}}
string(6) "parent"
string(5) "child"
{"ShadowBase":{"name":["parent"]},"ShadowProtected":{"name":["child"]}}
{"ShadowBase":{"name":["parent"]},"ShadowProtected":{"name":["child"]}}
string(6) "parent"
string(5) "child"
