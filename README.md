SecurePad
=========

SecurePad can be used to securely encrypt your documents.

SecurePad can be used, among other things for:

- Encrypting files on your hard drive with information like passwords, bank details, and other sensitive information.

- Sending sensitive information across the internet, such as through email.

SecurePad uses the powerful and secure [Blowfish cipher](https://en.wikipedia.org/wiki/Blowfish_(cipher)), though it is always advised that you use a reasonably complex password. It also means you should be careful because there is no way to recover your information if you forget the key you used!

Changelog
=========

v2.5
----
Auto encrypt/decrypt for `.ctxt` files:
- Opening a `.ctxt` file prompts for the key and decrypts the buffer for editing.
- Saving a `.ctxt` file encrypts on disk while the editor keeps the plaintext view.
- New menu item **Encrypt && Save As .ctxt...** prompts for a path + key, then saves the current document as an encrypted `.ctxt` file in one step.

v2.4
----
Fixed issue #13, regression form PR #12, completely replacing functionality with just the npp template code
Adpated to size_t for documents greater 2GB

v2.3
----

Recompiled with latest plugin files for compatibility with latest Notepad++ (8.3)
Added arm64 version
Updated to VS2022

v2.2
----

Recompiled with latest plugin files for compatibility with latest Notepad++ (6.5.1). Note that the compatibility break may have occurred before this. I have only tested that it works with this version, though it is likely to work with Notepad++ versions before 6.5 too.

v2.1
----

Security fix introduced in 2.0 (do not use 2.0!)

v2
--

Updated to Unicode

v1
--

Initial release
