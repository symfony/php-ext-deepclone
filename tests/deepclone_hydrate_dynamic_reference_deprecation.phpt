--TEST--
deepclone_hydrate() reports the dynamic properties it creates when it binds references to them
--EXTENSIONS--
deepclone
--FILE--
<?php

class Declared
{
    public $a;
}

#[AllowDynamicProperties]
class Dynamic
{
    public $a;
}

// Binding a reference to a new dynamic property is reported like writing a value
$v = 1;
$vars = ['dyn' => &$v];
$o = deepclone_hydrate(new Declared(), $vars, DEEPCLONE_HYDRATE_PRESERVE_REFS);
$v = 2;
var_dump($o->dyn);

// Not when the property exists already
deepclone_hydrate($o, $vars, DEEPCLONE_HYDRATE_PRESERVE_REFS);

// Nor when the class allows dynamic properties
deepclone_hydrate(new Dynamic(), $vars, DEEPCLONE_HYDRATE_PRESERVE_REFS);
deepclone_hydrate(new stdClass(), $vars, DEEPCLONE_HYDRATE_PRESERVE_REFS);

// Like other writes, the property is created even when an error handler throws
set_error_handler(function (int $type, string $message) { throw new ErrorException($message); });
$o = new Declared();
try {
    deepclone_hydrate($o, $vars, DEEPCLONE_HYDRATE_PRESERVE_REFS);
} catch (ErrorException $e) {
    echo $e->getMessage(), "\n";
}
var_dump($o);

?>
--EXPECTF--

Deprecated: Creation of dynamic property Declared::$dyn is deprecated in %s on line 17
int(2)
Creation of dynamic property Declared::$dyn is deprecated
object(Declared)#%d (2) {
  ["a"]=>
  NULL
  ["dyn"]=>
  &int(2)
}
