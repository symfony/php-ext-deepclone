--TEST--
deepclone_to_array() / deepclone_from_array() handles legacy Serializable interface
--EXTENSIONS--
deepclone
--FILE--
<?php

class LegacySerializable implements Serializable
{
    public function __construct(public string $data = '') {}

    public function serialize(): string
    {
        return $this->data;
    }

    public function unserialize(string $data): void
    {
        $this->data = $data;
    }
}

$obj = new LegacySerializable('hello-world');
$d = deepclone_to_array($obj);

// The class entry in the payload is the serialized form (starts with C: or O:)
$className = is_string($d['classes']) ? $d['classes'] : $d['classes'][0];
var_dump(str_contains($className, ':'));

$clone = deepclone_from_array($d);
var_dump($clone instanceof LegacySerializable);
var_dump($clone->data === 'hello-world');
var_dump($clone !== $obj);

echo "Done\n";
?>
--EXPECTF--
%Abool(true)
bool(true)
bool(true)
bool(true)
Done
