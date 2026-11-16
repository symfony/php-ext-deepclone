--TEST--
deepclone_from_array() checks objects against the type of properties without converting them, like unserialize()
--EXTENSIONS--
deepclone
--FILE--
<?php

// Like unserialize(), deepclone_from_array() doesn't convert objects to strings:
// __toString() would run on an object that might not be hydrated yet

class Box
{
    public string $label = '';
    public int $count = 0;
}

class Label
{
    public $text = 'default';
    public function __toString(): string { echo "__toString() called\n"; return $this->text; }
}

$payload = [
    'classes' => ['Box', 'Label'],
    'objectMeta' => [0, 1],
    'prepared' => 0,
    'properties' => ['stdClass' => ['label' => [0 => 1], 'text' => [1 => 'hydrated'], 'count' => [0 => '42']]],
    'resolve' => ['stdClass' => ['label' => [0 => true]]],
];

try {
    deepclone_from_array($payload);
} catch (TypeError $e) {
    echo $e->getMessage(), "\n";
}

try {
    unserialize('O:3:"Box":1:{s:5:"label";O:5:"Label":0:{}}');
} catch (TypeError $e) {
    echo $e->getMessage(), "\n";
}

// Scalars are still converted
unset($payload['properties']['stdClass']['label'], $payload['resolve']);
var_dump(deepclone_from_array($payload)->count);

// deepclone_hydrate() converts objects, like an assignment does
var_dump(deepclone_hydrate(new Box(), ['label' => new Label()])->label);

?>
--EXPECTF--
Cannot assign Label to property Box::$label of type string
Cannot assign Label to property Box::$label of type string
int(42)
__toString() called
string(7) "default"
