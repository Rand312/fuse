## Process this file with automake to produce Makefile.in

SUBDIRS = @subdirs@ @subdirs2@

EXTRA_DIST =			\
	fuse.pc.in		\
	README*			\
	Filesystems		\
	FAQ			\
	doc/how-fuse-works

pkgconfigdir = $(libdir)/pkgconfig
pkgconfig_DATA = fuse.pc

$(pkgconfig_DATA): config.status
