--TEST--
deepclone_to_array() leaves out the default values that are constant expressions, also for classes cached by OPcache
--EXTENSIONS--
deepclone
opcache
--INI--
opcache.enable=1
opcache.enable_cli=1
opcache.file_update_protection=0
--FILE--
<?php

const GLOBAL_DEFAULT = 'g';

enum Suit: string
{
    case Hearts = 'H';
}

class Conf
{
    const DEF = 5;

    public $plain = 1;
    public $viaConst = self::DEF;
    public $viaGlobal = GLOBAL_DEFAULT;
    public $enum = Suit::Hearts;
    public $changed = 0;
}

$o = new Conf();
$o->changed = 1;

var_dump(opcache_is_script_cached(__FILE__));
var_dump(deepclone_to_array($o)['properties']);

?>
--EXPECT--
bool(true)
array(1) {
  ["stdClass"]=>
  array(1) {
    ["changed"]=>
    array(1) {
      [0]=>
      int(1)
    }
  }
}
