# Security Policy

## Reporting a vulnerability

This extension is part of the Symfony ecosystem and follows the
[Symfony security process](https://symfony.com/security).

If you discover a security issue in this extension, **do not open a public
GitHub issue**. Instead, please report it privately by emailing
**security@symfony.com**.

You will receive a response within 48 hours. If the issue is confirmed, a fix
will be prepared and a coordinated disclosure scheduled.

## Supported versions

Only the latest minor release of this extension receives security updates
during the `0.x` development phase. Once `1.0` is tagged, the supported
versions table here will be updated to reflect a stable support window.

## Threat model

`deepclone_to_array()` is intended to be called on values produced by trusted
PHP code in the same process. It is **not** designed as a sandbox: it executes
`__sleep`, `__serialize`, and other magic methods on the values it walks, and
will reach any class autoloader configured in the process.

`deepclone_from_array()` validates the structure of the payload it is given
and throws `\ValueError` on malformed input. It instantiates classes named in
the payload via the standard PHP class loader. Treat its input the same way
you would treat input to `unserialize()`: only call it on payloads from
trusted sources.

### Closures

A by-name closure payload (a first-class callable over a named function or
method, e.g. `strlen(...)` or `Cls::method(...)`) is a stronger primitive
than anything `unserialize()` exposes: resolving it mints a `Closure` over
the named callable, with no visibility or staticness restriction, including
internal functions such as `system()`. Creating the closure does not call it,
but a payload pairing such a closure with an object whose `__wakeup`,
`__unserialize` or destructor invokes a stored callback turns it into a
one-step gadget. This encoding is therefore **off by default** and gated by
the `$allow_named_closures` flag, which both `deepclone_to_array()` and
`deepclone_from_array()` must set: a default `deepclone_from_array()` call
rejects any payload carrying a by-name closure before instantiating anything.
Enable it only between ends that trust each other.

Closures declared in constant expressions (the attribute-cache use case)
carry no such risk and need no opt-in: they serialize as a reference to their
declaration site and resolve only to a closure the named class itself
declares, the same bounded capability as unserializing a class or enum-case
name. The `$allowed_classes` filter still applies to both forms: omit
`Closure` from the list and no closure resolves at all.
