How to do a SPICE server release
================================

Some notes to prepare a release, not really strict but better to have in order
to avoid forgetting something.

* Update `configure.ac` and `meson.build` according to libtool rules
* Update `CHANGELOG.md` with list of changes done since last release
* Send a merge request with such changes, handle the review
* Try to build an RPM package from `make dist` output (see below notes
  on how to build a source tarball)
* `git push` for the above MR
* `git push` for the version tag created (for instance you can use
  `git push origin v0.14.3` or `git push --tags`)
* Send an email to spice-devel mailing list
* Sign generated tarball (to create a detached signature run
  `gpg2 -sb spice-release.tar.bz2`)
* On Gitlab update tags (https://gitlab.freedesktop.org/spice/spice/-/tags)
  * Add ChangeLog information
  * Upload tarball and relative signature
* Upload tarball and relative signature to
  https://www.spice-space.org/download/releases/ (sftp to
  `spice-uploader@spice-web.osci.io:/var/www/www.spice-space.org/`)
* Update file `download.rst` in
  https://gitlab.freedesktop.org/spice/spice-space-pages
* Create a merge request for `spice-space-pages`

How to build a source tarball
-----------------------------

1. `git tag -a v0.14.3 -m 'spice-server 0.14.3'` (replace the version)
2. `git clone https://gitlab.freedesktop.org/spice/spice.git`
3. `cd spice`
4. `./autogen.sh --disable-dependency-tracking`
5. `(cd subprojects/spice-common && make gitignore)`
6. `make dist`

Step 4 is to avoid a problem with `.deps` directories, step 5 to avoid
the directory to be dirty due to missing `.gitignore` files.
