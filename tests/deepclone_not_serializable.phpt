--TEST--
Classes that refuse serialization are rejected whatever methods they declare, except anonymous ones that restore their state
--EXTENSIONS--
deepclone
--INI--
error_reporting=E_ALL & ~E_DEPRECATED
--FILE--
<?php

// SplFileInfo refuses serialization, which its subclasses inherit: serialize()
// and unserialize() reject them whatever methods they declare
class FileInfoSleep extends SplFileInfo
{
    public function __sleep(): array { return []; }
}
class FileInfoWakeup extends SplFileInfo
{
    public function __wakeup(): void {}
}
class FileInfoSerialize extends SplFileInfo
{
    public function __serialize(): array { return []; }
}
class FileInfoUnserialize extends SplFileInfo
{
    public function __unserialize(array $data): void {}
}
class FileInfoBoth extends SplFileInfo
{
    public function __serialize(): array { return ['path' => $this->getPathname()]; }
    public function __unserialize(array $data): void { parent::__construct($data['path']); }
}
class FileInfoSerializable extends SplFileInfo implements Serializable
{
    public function serialize(): string { return $this->getPathname(); }
    public function unserialize(string $data): void { parent::__construct($data); }
}

function check(string $label, callable $fn): void
{
    try {
        $fn();
        echo "$label: accepted\n";
    } catch (Throwable $e) {
        echo "$label: ", $e::class, ': ', $e->getMessage(), "\n";
    }
}

echo "== deepclone_to_array()\n";
foreach ([FileInfoSleep::class, FileInfoWakeup::class, FileInfoSerialize::class, FileInfoUnserialize::class, FileInfoBoth::class, FileInfoSerializable::class] as $class) {
    check($class, fn () => deepclone_to_array(new $class(__FILE__)));
}

// Anonymous classes refuse serialization too, but they round-trip in-process
// when they restore their state with __wakeup() or __unserialize(), like
// throwables do
$anonymous = [
    'none' => new class { public $a = 1; },
    '__sleep' => new class { public $a = 1; public function __sleep(): array { return ['a']; } },
    '__serialize' => new class { public $a = 1; public function __serialize(): array { return ['a' => $this->a]; } },
    'Serializable' => new class implements Serializable { public function serialize(): string { return ''; } public function unserialize(string $data): void {} },
    '__wakeup' => new class { public $a = 1; public function __wakeup(): void {} },
    '__unserialize' => new class { public $a = 1; public function __serialize(): array { return ['a' => $this->a]; } public function __unserialize(array $data): void { $this->a = $data['a']; } },
    'exception' => new class('boom') extends Exception {},
    'subclass' => new class(__FILE__) extends SplFileInfo { public function __unserialize(array $data): void {} },
];
foreach ($anonymous as $label => $object) {
    check("anonymous $label", fn () => deepclone_from_array(deepclone_to_array($object)));
}

echo "== deepclone_from_array()\n";
foreach ([FileInfoWakeup::class, SplFileInfo::class, Closure::class, Generator::class, $anonymous['none']::class] as $class) {
    check($class, fn () => deepclone_from_array(['classes' => $class, 'objectMeta' => 1, 'prepared' => 0]));
}

echo "== deepclone_hydrate()\n";
foreach ([FileInfoWakeup::class, FileInfoBoth::class, $anonymous['none']::class, $anonymous['__unserialize']::class, $anonymous['exception']::class] as $class) {
    check($class, fn () => deepclone_hydrate($class));
}
?>
--EXPECTF--
== deepclone_to_array()
FileInfoSleep: DeepClone\NotInstantiableException: Type "FileInfoSleep" is not instantiable.
FileInfoWakeup: DeepClone\NotInstantiableException: Type "FileInfoWakeup" is not instantiable.
FileInfoSerialize: DeepClone\NotInstantiableException: Type "FileInfoSerialize" is not instantiable.
FileInfoUnserialize: DeepClone\NotInstantiableException: Type "FileInfoUnserialize" is not instantiable.
FileInfoBoth: DeepClone\NotInstantiableException: Type "FileInfoBoth" is not instantiable.
FileInfoSerializable: DeepClone\NotInstantiableException: Type "FileInfoSerializable" is not instantiable.
anonymous none: DeepClone\NotInstantiableException: Type "class@anonymous" is not instantiable.
anonymous __sleep: DeepClone\NotInstantiableException: Type "class@anonymous" is not instantiable.
anonymous __serialize: DeepClone\NotInstantiableException: Type "class@anonymous" is not instantiable.
anonymous Serializable: DeepClone\NotInstantiableException: Type "Serializable@anonymous" is not instantiable.
anonymous __wakeup: accepted
anonymous __unserialize: accepted
anonymous exception: accepted
anonymous subclass: DeepClone\NotInstantiableException: Type "SplFileInfo@anonymous" is not instantiable.
== deepclone_from_array()
FileInfoWakeup: DeepClone\NotInstantiableException: Type "FileInfoWakeup" is not instantiable.
SplFileInfo: DeepClone\NotInstantiableException: Type "SplFileInfo" is not instantiable.
Closure: DeepClone\NotInstantiableException: Type "Closure" is not instantiable.
Generator: DeepClone\NotInstantiableException: Type "Generator" is not instantiable.
class@anonymous%0%s: DeepClone\NotInstantiableException: Type "class@anonymous" is not instantiable.
== deepclone_hydrate()
FileInfoWakeup: DeepClone\NotInstantiableException: Type "FileInfoWakeup" is not instantiable.
FileInfoBoth: DeepClone\NotInstantiableException: Type "FileInfoBoth" is not instantiable.
class@anonymous%0%s: DeepClone\NotInstantiableException: Type "class@anonymous" is not instantiable.
class@anonymous%0%s: accepted
Exception@anonymous%0%s: accepted
