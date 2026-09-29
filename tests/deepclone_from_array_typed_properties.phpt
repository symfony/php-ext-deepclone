--TEST--
deepclone_from_array() writes typed properties like unserialize(), without the conveniences of deepclone_hydrate()
--EXTENSIONS--
deepclone
--FILE--
<?php

enum Suit: string { case Hearts = 'H'; }

class Typed
{
    public int $i;
    public Suit $s;
}

foreach ([['i' => [null]], ['s' => ['H']]] as $props) {
    try {
        deepclone_from_array(['classes' => 'Typed', 'objectMeta' => 1, 'prepared' => 0, 'properties' => ['stdClass' => $props]]);
    } catch (TypeError $e) {
        echo $e->getMessage(), "\n";
    }
}

// deepclone_hydrate() leaves the property uninitialized and casts the scalar
$o = deepclone_hydrate('Typed', ['i' => null, 's' => 'H']);
var_dump(isset($o->i), $o->s);
?>
--EXPECT--
Cannot assign null to property Typed::$i of type int
Cannot assign string to property Typed::$s of type Suit
bool(false)
enum(Suit::Hearts)
