# Copyright (C) 2002, 2004 Free Software Foundation, Inc.
#
# This file is part of GnuPG.
#
# GnuPG is free software; you can redistribute it and/or modify
# it under the terms of the GNU General Public License as published by
# the Free Software Foundation; either version 3 of the License, or
# (at your option) any later version.
#
# GnuPG is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU General Public License for more details.
#
# You should have received a copy of the GNU General Public License
# along with this program; if not, see <https://www.gnu.org/licenses/>.

## Process this file with automake to produce Makefile.in

AM_CPPFLAGS =

include $(top_srcdir)/am/cmacros.am

examples = examples/README examples/scd-event examples/trustlist.txt	\
           examples/qualified.txt                                       \
	   examples/common.conf                                         \
           examples/gpgconf.rnames examples/gpgconf.conf                \
	   examples/pwpattern.list

helpfiles = help.txt help.be.txt help.ca.txt help.cs.txt		\
            help.da.txt help.de.txt help.el.txt help.eo.txt		\
            help.es.txt help.et.txt help.fi.txt help.fr.txt		\
            help.gl.txt help.hu.txt help.id.txt help.it.txt		\
            help.ja.txt help.nb.txt help.pl.txt help.pt.txt		\
            help.pt_BR.txt help.ro.txt help.ru.txt help.sk.txt		\
            help.sv.txt help.tr.txt help.zh_CN.txt help.zh_TW.txt       \
            mail-tube.txt mail-tube.de.txt \
            wks-utils.txt wks-utils.de.txt

EXTRA_DIST = samplekeys.asc mksamplekeys com-certs.pem \
	     gnupg-logo.pdf gnupg-logo.png gnupg-logo-tr.png \
	     gnupg-module-overview.png gnupg-module-overview.pdf \
             gnupg-card-architecture.png gnupg-card-architecture.pdf \
             FAQ gnupg7.texi mkdefsinc.c defsincdate \
             opt-homedir.texi see-also-note.texi specify-user-id.texi \
	     gpgv.texi ChangeLog-2011 whats-new-in-2.1.txt \
             trust-values.texi keyformat.txt

BUILT_SOURCES = gnupg-module-overview.png gnupg-module-overview.pdf \
                gnupg-card-architecture.png gnupg-card-architecture.pdf \
                defsincdate defs.inc

info_TEXINFOS = gnupg.texi

dist_pkgdata_DATA =  $(helpfiles)

nobase_dist_doc_DATA = FAQ DETAILS HACKING DCO TRANSLATE OpenPGP KEYSERVER \
                       $(examples)

#dist_html_DATA =


gnupg_TEXINFOS = \
	gpg.texi gpgsm.texi gpg-agent.texi scdaemon.texi instguide.texi \
	tools.texi debugging.texi glossary.texi contrib.texi gpl.texi \
	sysnotes.texi dirmngr.texi wks.texi gpg-card.texi \
        gnupg-module-overview.svg \
        gnupg-card-architecture.fig \
	howtos.texi howto-create-a-server-cert.texi

gnupg.texi : defs.inc


DVIPS = TEXINPUTS="$(srcdir)$(PATH_SEPARATOR)$$TEXINPUTS" dvips

AM_MAKEINFOFLAGS = -I $(srcdir) --css-ref=/share/site.css

YAT2M_OPTIONS = -I $(srcdir) \
        --release "GnuPG @PACKAGE_VERSION@" --source "GNU Privacy Guard 2.6"

myman_sources = gnupg7.texi gpg.texi gpgsm.texi gpg-agent.texi \
	        dirmngr.texi scdaemon.texi tools.texi wks.texi \
                gpg-card.texi
myman_pages   = gpg.1 gpgv.1 gpgsm.1 gpg-agent.1 dirmngr.8 scdaemon.1 \
                watchgnupg.1 gpgconf.1 addgnupghome.8 gpg-preset-passphrase.1 \
		gpg-connect-agent.1 gpgparsemail.1 gpgtar.1 gpg-mail-tube.1 \
		applygnupgdefaults.8 gpg-wks-client.1 gpg-wks-server.1 \
		dirmngr-client.1 gpg-card.1 gpg-check-pattern.1
myhtmlman_pages = \
                gpg.1.html gpgv.1.html gpgsm.1.html \
                gpg-agent.1.html dirmngr.8.html scdaemon.1.html \
                watchgnupg.1.html gpgconf.1.html addgnupghome.8.html \
                gpg-preset-passphrase.1.html \
		gpg-connect-agent.1.html gpgparsemail.1.html \
                gpgtar.1.html gpg-mail-tube.1.html \
		applygnupgdefaults.8.html gpg-wks-client.1.html \
                gpg-wks-server.1.html \
		dirmngr-client.1.html gpg-card.1.html gpg-check-pattern.1.html \
		gnupg.7.html

man_MANS = $(myman_pages) gnupg.7

watchgnupg_SOURCE = gnupg.texi


CLEANFILES = mkdefsinc defs.inc

DISTCLEANFILES = gnupg.tmp gnupg.ops yat2m-stamp.tmp yat2m-stamp \
		 $(myman_pages) gnupg.7 $(myhtmlman_pages)

mkdefsinc: mkdefsinc.c Makefile ../config.h
	$(CC_FOR_BUILD) -I. -I.. -I$(srcdir) $(AM_CPPFLAGS) \
                        -o $@ $(srcdir)/mkdefsinc.c

if MAINTAINER_MODE
.svg.eps:
	convert `test -f '$<' || echo '$(srcdir)/'`$< $@

.svg.png:
	convert `test -f '$<' || echo '$(srcdir)/'`$< $@

.svg.pdf:
	convert `test -f '$<' || echo '$(srcdir)/'`$< $@

.fig.png:
	fig2dev -L png `test -f '$<' || echo '$(srcdir)/'`$< $@

.fig.jpg:
	fig2dev -L jpeg `test -f '$<' || echo '$(srcdir)/'`$< $@

.fig.eps:
	fig2dev -L eps `test -f '$<' || echo '$(srcdir)/'`$< $@

.fig.pdf:
	fig2dev -L pdf `test -f '$<' || echo '$(srcdir)/'`$< $@
endif


yat2m-stamp: $(myman_sources) defs.inc
	@rm -f yat2m-stamp.tmp
	@touch yat2m-stamp.tmp
	incd="`test -f defsincdate || echo '$(srcdir)/'`defsincdate"; \
	for file in $(myman_sources); do \
              $(YAT2M) $(YAT2M_OPTIONS) --store \
                  --date "`cat $$incd 2>/dev/null`" \
	          `test -f '$$file' || echo '$(srcdir)/'`$$file ;\
              $(YAT2M) $(YAT2M_OPTIONS) --store --html --gnupgorg \
                  --date "`cat $$incd 2>/dev/null`" \
	          `test -f '$$file' || echo '$(srcdir)/'`$$file ;\
        done
	@mv -f yat2m-stamp.tmp $@


$(myman_pages) $(myhtmlman_pages) gnupg.7 : yat2m-stamp defs.inc
	@if test -f $@; then :; else \
            trap 'rm -rf yat2m-stamp yat2m-lock' 1 2 13 15; \
               if mkdir yat2m-lock 2>/dev/null; then \
                 rm -f yat2m-stamp; \
                 $(MAKE) $(AM_MAKEFLAGS) yat2m-stamp; \
                 rmdir yat2m-lock; \
               else \
                 while test -d yat2m-lock; do sleep 1; done; \
                 test -f yat2m-stamp; exit $$?; \
               fi; \
             fi

dist-hook: defsincdate

defsincdate: $(gnupg_TEXINFOS)
	: >defsincdate ; \
	if test -e $(top_srcdir)/.git; then \
	  (cd $(srcdir) && git log -1 --format='%ct' \
               -- $(gnupg_TEXINFOS) 2>/dev/null) >>defsincdate; \
        elif test x"$$SOURCE_DATE_EPOCH" != x; then   \
	   echo "$$SOURCE_DATE_EPOCH" >>defsincdate ; \
	fi

defs.inc : defsincdate Makefile mkdefsinc
	incd="`test -f defsincdate || echo '$(srcdir)/'`defsincdate"; \
	./mkdefsinc -C $(srcdir) --date "`cat $$incd 2>/dev/null`" \
	    $(gnupg_TEXINFOS) >$@


online: gnupg.html gnupg.pdf gnupg-module-overview.png \
           gnupg-card-architecture.png
	set -e; \
	echo "Uploading current manuals to www.gnupg.org ..."; \
	cp $(srcdir)/gnupg-logo-tr.png gnupg.html/; \
	cp gnupg-module-overview.png gnupg.html/; \
	cp gnupg-card-architecture.png gnupg.html/; \
        user=werner ; webhost="ftp.gnupg.org" ; dashdevel="" ; \
        if echo "@PACKAGE_VERSION@" | grep -- "-beta" >/dev/null; then \
	  dashdevel="-devel" ; \
	else \
          rsync -v gnupg.pdf $${user}@$${webhost}:webspace/manuals/ ; \
        fi ; \
	cd gnupg.html ; \
        rsync -vr --exclude='.git' .  \
	  $${user}@$${webhost}:webspace/manuals/gnupg$${dashdevel}/
