###########################################################################
### Rules
###

### Default tag
help:
	@echo "Targets:"
	@echo " build         -- build all of valgrind and fjalar (includes kvasir)"
	@echo " test          -- build and run valgrind regression tests"
	@echo "               -- see the Daikon tree for fjalar/kvasir regression tests"
	@echo " clean         -- remove basic generated files"
	@echo " very-clean    -- remove (most) all generated files"
	@echo " valgrind-merge-docs -- documentation for doing a Valgrind merge"
	@echo " hg-update     -- add/remove files as result of Valgrind merge"
	@echo " "

build:
	bash ./auto-everything.sh

test:
	cd valgrind && $(MAKE) regtest

clean:
	cd valgrind && $(MAKE) clean

very-clean:
	cd valgrind/docs && $(MAKE) distclean
	cd valgrind && $(MAKE) uninstall
	cd valgrind && $(MAKE) distclean
	rm -rf valgrind/config.h.in
	rm -rf valgrind/aclocal.m4
	rm -rf valgrind/configure
	rm -rf valgrind/autom4te.cache
	rm -rf valgrind/Makefile.in
	rm -rf valgrind/Makefile.vex.in

.PHONY: valgrind-merge-docs
valgrind-merge-docs: valgrind-merge.info valgrind-merge.html valgrind-merge.pdf

valgrind-merge.info: valgrind-merge.texinfo
	makeinfo $<

valgrind-merge.pdf: valgrind-merge.texinfo
	makeinfo --pdf $<

valgrind-merge.html: valgrind-merge.texinfo
	makeinfo --html --no-split $<

hg-update:
	@grep -q '^\-\-\-.*1969\-12\-31' ../coregrind.patch; \
	if [ $$? -eq 0 ]; then \
	  echo hg add `grep '^\-\-\-.*1969\-12\-31' ../coregrind.patch | cut --fields=1 | cut -d ' ' --fields=2 | perl -p -e 's/^valgrind-old/valgrind/g'`; \
	fi
	
	@grep -q '^Only in valgrind-old' ../coregrind.patch; \
	if [ $$? -eq 0 ]; then \
	  echo hg remove `grep '^Only in valgrind-old' ../coregrind.patch | perl -p -e 's/: /\//g' | cut -d ' ' --fields=3 | perl -p -e 's/^valgrind-old/valgrind/g'`; \
	fi
	
	@grep -q '^\-\-\-.*1969\-12\-31' ../memcheck.patch; \
	if [ $$? -eq 0 ]; then \
	  echo add `grep '^\-\-\-.*1969\-12\-31' ../memcheck.patch| cut --fields=1 | cut -d ' ' --fields=2 | perl -p -e 's/^valgrind-old/valgrind/g'`; \
	fi
	
	@grep -q '^\+\+\+.*1969\-12\-31' ../memcheck.patch; \
	if [ $$? -eq 0 ]; then \
	  echo remove `grep '^\+\+\+.*1969\-12\-31' ../memcheck.patch| cut --fields=1 | cut -d ' ' --fields=2 | perl -p -e 's/^valgrind-new/valgrind/g'`; \
	fi

