--TEST--
deepclone_hydrate() loads the backed enum of a property to convert a scalar to its case
--EXTENSIONS--
deepclone
--FILE--
<?php

spl_autoload_register(function (string $class) {
    echo "autoloading $class\n";
    if ('Status' === $class) {
        eval('enum Status: string { case Active = "active"; }');
    }
});

class User
{
    public Status $status;
    public ?Missing $missing = null;
    public ?self $manager = null;
}

var_dump(deepclone_hydrate(User::class, ['status' => 'active'])->status);

foreach (['missing', 'manager'] as $name) {
    try {
        deepclone_hydrate(User::class, [$name => 'x']);
    } catch (TypeError $e) {
        echo $e->getMessage(), "\n";
    }
}

?>
--EXPECTF--
autoloading Status
enum(Status::Active)
autoloading Missing
Cannot assign string to property User::$missing of type ?Missing
Cannot assign string to property User::$manager of type ?%s
