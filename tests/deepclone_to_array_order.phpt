--TEST--
deepclone_to_array() lists properties and their markers object by object, in the order of their ids, like the polyfill
--EXTENSIONS--
deepclone
--FILE--
<?php

class Node
{
    public $next;
    public $data;
}

// The names of an object's properties come before the ones of the objects it
// references: deepclone_from_array() creates dynamic properties in this order
$b = new stdClass();
$b->c = 3;
$b->b = 2;
$a = new stdClass();
$a->a = $b;
$a->b = 1;
echo json_encode(deepclone_to_array($a)['properties']), "\n";

// Markers come in the same order, the ones of an object before the ones of
// the objects it references
$n1 = new Node();
$n2 = new Node();
$n3 = new Node();
$n1->next = $n2;
$n1->data = [$n3];
$n2->next = $n3;
$n3->next = $n1;
$n3->data = [$n2];
$payload = deepclone_to_array($n1);
echo json_encode($payload['properties']), "\n";
echo json_encode($payload['resolve']), "\n";

// A reference found once needs no marker, but still places the name it's the
// value of
$v = 1;
$n1 = new Node();
$n1->next = &$v;
$n2 = new Node();
$n2->data = new stdClass();
$n3 = new Node();
$n3->next = new stdClass();
$payload = deepclone_to_array([$n1, $n2, $n3]);
echo json_encode($payload['properties']), "\n";
echo json_encode($payload['resolve']), "\n";
?>
--EXPECT--
{"stdClass":{"a":[1],"b":[1,2],"c":{"1":3}}}
{"stdClass":{"next":[1,2,0],"data":{"0":[2],"2":[1]}}}
{"stdClass":{"next":[true,true,true],"data":{"0":[true],"2":[true]}}}
{"stdClass":{"next":{"0":1,"3":4},"data":{"1":2}}}
{"stdClass":{"next":{"3":true},"data":{"1":true}}}
