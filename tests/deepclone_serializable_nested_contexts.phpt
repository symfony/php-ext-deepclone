--TEST--
Native serialization runs in its own context when called from Serializable::serialize() or ::unserialize()
--EXTENSIONS--
deepclone
--FILE--
<?php

// Implementing Serializable is deprecated
error_reporting(E_ALL & ~E_DEPRECATED);

class Leaf implements Serializable
{
    public $data = 'leaf';
    public function serialize() { return $this->data; }
    public function unserialize($data) { $this->data = $data; }
}

class Wrapper implements Serializable
{
    public static $payload;
    public $inner;

    public function serialize()
    {
        return json_encode(self::$payload = deepclone_to_array($this->inner));
    }

    public function unserialize($data)
    {
        $this->inner = deepclone_from_array(json_decode($data, true));
    }
}

// Inside Serializable::serialize(), the payload doesn't reference the stream of the outer serialize() call
$leaf = new Leaf();
$wrapper = new Wrapper();
$wrapper->inner = [$leaf, $leaf];
$serialized = serialize([$leaf, $wrapper, $leaf]);
echo $serialized, "\n";
var_dump(Wrapper::$payload === deepclone_to_array([$leaf, $leaf]));

// and it restores the same outside of the outer unserialize() call
$inner = deepclone_from_array(Wrapper::$payload);
var_dump($inner[0]->data, $inner[0] === $inner[1]);

// Inside Serializable::unserialize(), the outer call keeps its own numbering
$clone = unserialize($serialized);
var_dump($clone[0] === $clone[2], $clone[1]->inner[0] === $clone[1]->inner[1], $clone[1]->inner[0] === $clone[0], $clone[1]->inner[0]->data);

// A payload can't reach the values of the outer call
try {
    unserialize('a:2:{i:0;O:8:"stdClass":0:{}i:1;C:7:"Wrapper":46:{{"classes":"r:2;","objectMeta":1,"prepared":0}}}');
} catch (ValueError $e) {
    echo $e->getMessage(), "\n";
}

// A back-reference of the outer stream after the nested call
$payload = json_encode(deepclone_to_array(new Leaf()));
$tail = 'R:3;';
for ($i = 0; $i < 8; ++$i) {
    $tail = "a:1:{i:0;{$tail}}";
}
var_dump(is_array(@unserialize('a:2:{i:0;C:7:"Wrapper":'.strlen($payload).':{'.$payload.'}i:1;'.$tail.'}')));

?>
--EXPECT--
a:3:{i:0;C:4:"Leaf":4:{leaf}i:1;C:7:"Wrapper":86:{{"classes":"C:4:\"Leaf\":4:{leaf}","objectMeta":1,"prepared":[0,0],"mask":[true,true]}}i:2;r:2;}
bool(true)
string(4) "leaf"
bool(true)
bool(true)
bool(true)
bool(false)
string(4) "leaf"
deepclone_from_array(): Argument #1 ($data) failed to unserialize object 0
bool(true)
