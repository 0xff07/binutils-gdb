###
### Makefile used to three-stage build a tree of source code.  Also used to
### compile other bundles, first with cc, then with gcc.
###

###
### USE OF THIS FILE REQUIRES GNU MAKE!!!
###

### The first versions of the file were written by Rich Pixley (rich@cygnus.com).
### Many subsequent additions (and current maintainance by) david d `zoo' zuhn,
### (zoo@cygnus.com).

### Every invocation of this Makefile needs to have a variable set (host), 
### which is the named used for ./configure, and also the prefix for the
### various files and directories used in a three stage.

ifndef host
error:
	@echo You must set the variable \"host\" to use this Makefile ; exit 1
else

### from here to very near the end of the file is the real guts of this 
### Makefile, and it is not seen if the variable 'host' is not set 

###
### START EDITTING HERE!!!
### These things will need to be set differently for each release.
###

### from which cvs tree are we working?
TREE := devo

### binaries should be installed into?
ROOTING := /usr/cygnus

### When working from a tagged set of source, this should be the tag.  If not,
### then set the macro to be empty.
CVS_TAG := 

### The name of the cvs module for this release.  The intersection of
### CVS_MODULE and CVS_TAG defines the source files in this release.
CVS_MODULE := latest

### Historically, this was identical to CVS_TAG.  This is changing.
RELEASE_TAG := latest-921229

### Historically, binaries were installed here.  This is changing.
release_root := $(ROOTING)/$(RELEASE_TAG)

### STOP EDITTING HERE!!!
### With luck, eventually, nothing else will need to be editted.

TIME 		:= time
GCC 		:= gcc -O -g
GNUC		:= "CC=$(GCC)"
CFLAGS		:= -g
GNU_MAKE 	:= /usr/latest/bin/make -w 

override MAKE 		:= make
override MFLAGS 	:=
#override MAKEFLAGS 	:=

SHELL := /bin/sh

FLAGS_TO_PASS := \
	"GCC=$(GCC)" \
	"CFLAGS=$(CFLAGS)" \
	"TIME=$(TIME)" \
	"MF=$(MF)" \
	"host=$(host)"


prefixes	= -prefix=$(release_root) -exec-prefix=$(release_root)/H-$(host)
relbindir	= $(release_root)/H-$(host)/bin


### general config stuff
WORKING_DIR 	:= $(host)-objdir
STAGE1DIR 	:= $(WORKING_DIR).1
STAGE2DIR 	:= $(WORKING_DIR).2
STAGE3DIR 	:= $(WORKING_DIR).3
INPLACEDIR 	:= $(host)-in-place
HOLESDIR 	:= $(host)-holes

.PHONY: all
ifdef target
##
## This is a cross compilation
##
arch 		= $(host)-x-$(target)
config  	= $(host) -target=$(target)
NATIVEDIR	:= $(arch)-native-objdir
CYGNUSDIR	:= $(arch)-cygnus-objdir
LATESTDIR	:= $(arch)-latest-objdir
FLAGS_TO_PASS	:= $(FLAGS_TO_PASS) "target=$(target)"

all:	do-native do-latest
build-all: build-native build-latest

else
##
## This is a native compilation
##
all:	$(host)-stamp-3stage-done
#all:	in-place do1 do2 do3 comparison
endif

everything: 	 do-cross 
#everything: 	in-place do1 do2 do3 comparison do-cygnus 

.PHONY: do-native
do-native: $(host)-stamp-holes $(arch)-stamp-native
do-native-config: $(arch)-stamp-native-configured
build-native: $(host)-stamp-holes $(arch)-stamp-native-checked
config-native: $(host)-stamp-holes $(arch)-stamp-native-configured

$(arch)-stamp-native:
	PATH=`pwd`/$(HOLESDIR) ; \
	  export PATH ; \
	  SHELL=sh ; export SHELL ; \
	  $(TIME) $(GNU_MAKE) -f test-build.mk  $(arch)-stamp-native-installed $(FLAGS_TO_PASS) 
	if [ -f CLEAN_ALL ] ; then rm -rf $(NATIVEDIR) ; else true ; fi
	touch $(arch)-stamp-native

$(arch)-stamp-native-installed: $(arch)-stamp-native-checked
	cd $(NATIVEDIR) ; $(TIME) $(MAKE) $(FLAGS_TO_PASS) install 
	cd $(NATIVEDIR) ; $(TIME) $(MAKE) $(FLAGS_TO_PASS) install-info 
	touch $@

$(arch)-stamp-native-checked: $(arch)-stamp-native-built
#	cd $(NATIVEDIR) ; $(TIME) $(MAKE) $(FLAGS_TO_PASS) check 
	touch $@

$(arch)-stamp-native-built: $(arch)-stamp-native-configured
	cd $(NATIVEDIR) ; $(TIME) $(MAKE) $(FLAGS_TO_PASS) all 
	cd $(NATIVEDIR) ; $(TIME) $(MAKE) $(FLAGS_TO_PASS) info 
	touch $@

$(arch)-stamp-native-configured:
	[ -d $(NATIVEDIR) ] || mkdir $(NATIVEDIR)
	(cd $(NATIVEDIR) ; \
		$(TIME) ../$(TREE)/configure $(config) -v -srcdir=../$(TREE) \
			$(prefixes))
	touch $@


.PHONY: do-cygnus
do-cygnus: $(host)-stamp-holes $(arch)-stamp-cygnus
build-cygnus: $(host)-stamp-holes $(arch)-stamp-cygnus-checked
config-cygnus: $(host)-stamp-holes $(arch)-stamp-cygnus-configured

$(arch)-stamp-cygnus: 
	[ -f $(relbindir)/gcc ] || (echo "must have gcc available"; exit 1)
	PATH=$(relbindir):`pwd`/$(HOLESDIR) ; \
	  export PATH ; \
	  SHELL=sh ; export SHELL ; \
	  echo ;  gcc -v ; echo ; \
	  $(TIME) $(GNU_MAKE) -f test-build.mk $(arch)-stamp-cygnus-installed  $(FLAGS_TO_PASS)
	if [ -f CLEAN_ALL ] ; then rm -rf $(CYGNUSDIR) ; else true ; fi
	touch $(arch)-stamp-cygnus

$(arch)-stamp-cygnus-installed: $(arch)-stamp-cygnus-checked
	cd $(CYGNUSDIR) ; $(TIME) $(MAKE) $(FLAGS_TO_PASS) $(GNUC) install 
	cd $(CYGNUSDIR) ; $(TIME) $(MAKE) $(FLAGS_TO_PASS) $(GNUC) install-info
	touch $@

$(arch)-stamp-cygnus-checked: $(arch)-stamp-cygnus-built
#	cd $(CYGNUSDIR) ; $(TIME) $(MAKE) $(FLAGS_TO_PASS) $(GNUC) check 
	touch $@

$(arch)-stamp-cygnus-built: $(arch)-stamp-cygnus-configured
	cd $(CYGNUSDIR) ; $(TIME) $(MAKE) $(FLAGS_TO_PASS) $(GNUC) all 
	cd $(CYGNUSDIR) ; $(TIME) $(MAKE) $(FLAGS_TO_PASS) $(GNUC) info 
	touch $@

$(arch)-stamp-cygnus-configured:
	[ -d $(CYGNUSDIR) ] || mkdir $(CYGNUSDIR)
	cd $(CYGNUSDIR) ; \
	  $(TIME) ../$(TREE)/configure $(config) -v -srcdir=../$(TREE) $(prefixes)
	touch $@

.PHONY: do-latest
do-latest: $(host)-stamp-holes $(arch)-stamp-latest
build-latest: $(host)-stamp-holes $(arch)-stamp-latest-checked

$(arch)-stamp-latest:
	PATH=/usr/latest/bin:`pwd`/$(HOLESDIR) ; \
	  export PATH ; \
	  SHELL=sh ; export SHELL ; \
	  $(TIME) $(GNU_MAKE) -f test-build.mk $(arch)-stamp-latest-installed  $(FLAGS_TO_PASS)
	touch $(arch)-stamp-latest

$(arch)-stamp-latest-installed: $(arch)-stamp-latest-checked
	cd $(LATESTDIR) ; $(TIME) $(MAKE) $(FLAGS_TO_PASS) $(GNUC) install 
	cd $(LATESTDIR) ; $(TIME) $(MAKE) $(FLAGS_TO_PASS) $(GNUC) install-info 
	touch $@

$(arch)-stamp-latest-checked: $(arch)-stamp-latest-built
#	cd $(LATESTDIR) ; $(TIME) $(MAKE) $(FLAGS_TO_PASS) $(GNUC) check 
	touch $@

$(arch)-stamp-latest-built: $(arch)-stamp-latest-configured
	cd $(LATESTDIR) ; $(TIME) $(MAKE) $(FLAGS_TO_PASS) $(GNUC) all 
	cd $(LATESTDIR) ; $(TIME) $(MAKE) $(FLAGS_TO_PASS) $(GNUC) info 
	touch $@

$(arch)-stamp-latest-configured:
	[ -d $(LATESTDIR) ] || mkdir $(LATESTDIR)
	cd $(LATESTDIR) ; \
	  $(TIME) ../$(TREE)/configure $(config) -v -srcdir=../$(TREE) $(prefixes)
	touch $@


.PHONY: in-place
in-place:	$(host)-stamp-in-place

$(host)-stamp-in-place: 
	PATH=/bin:/usr/bin:/usr/ucb ; \
	  export PATH ; \
	  SHELL=/bin/sh ; export SHELL ; \
	  $(TIME) $(GNU_MAKE) -f test-build.mk $(host)-stamp-in-place-installed host=$(host) $(FLAGS_TO_PASS)
	touch $@
	if [ -f CLEAN_ALL ] ; then \
	  rm -rf $(INPLACEDIR) ; \
 	else \
	  mv $(INPLACEDIR) $(STAGE1DIR) ; \
	fi

$(host)-stamp-in-place-installed: $(host)-stamp-in-place-checked
	cd $(INPLACEDIR) ; $(TIME) $(MAKE) $(MF) "CFLAGS=$(CFLAGS)" install host=$(host)
	cd $(INPLACEDIR) ; $(TIME) $(MAKE) $(MF) "CFLAGS=$(CFLAGS)" install-info host=$(host)
	touch $@

$(host)-stamp-in-place-checked: $(host)-stamp-in-place-built
#	cd $(INPLACEDIR) ; $(TIME) $(MAKE) $(MF) "CFLAGS=$(CFLAGS)" check host=$(host)
	touch $@

$(host)-stamp-in-place-built: $(host)-stamp-in-place-configured
	cd $(INPLACEDIR) ; $(TIME) $(MAKE) $(MF) "CFLAGS=$(CFLAGS)" all host=$(host)
	cd $(INPLACEDIR) ; $(TIME) $(MAKE) $(MF) "CFLAGS=$(CFLAGS)" info host=$(host)
	touch $@

$(host)-stamp-in-place-configured: $(host)-stamp-in-place-cp
	cd $(INPLACEDIR) ; $(TIME) ./configure $(host) -v $(prefixes)
	touch $@

$(host)-stamp-in-place-cp:
	rm -rf $(INPLACEDIR)
	mkdir $(INPLACEDIR)
	(cd $(TREE) ; tar cf - .) | (cd $(INPLACEDIR) ; tar xf -)
	touch $@

$(host)-stamp-3stage-done: do1 do2 do3 comparison
	touch $@


.PHONY: do1
do1:	$(host)-stamp-holes $(host)-stamp-stage1
do1-config: $(host)-stamp-stage1-configured
do1-build: $(host)-stamp-stage1-checked

$(host)-stamp-stage1:
	if [ -d $(STAGE1DIR) ] ; then \
		mv $(STAGE1DIR) $(WORKING_DIR) ; \
	else \
		true ; \
	fi
	PATH=`pwd`/$(HOLESDIR) ; \
	  export PATH ; \
	  SHELL=sh ; export SHELL ; \
	  $(TIME) $(GNU_MAKE) -f test-build.mk $(host)-stamp-stage1-installed host=$(host) $(FLAGS_TO_PASS) $(NATIVEC)
	touch $@
	if [ -f CLEAN_ALL ] ; then \
	  rm -rf $(WORKING_DIR) ; \
	else \
	  mv $(WORKING_DIR) $(STAGE1DIR) ; \
	fi

$(host)-stamp-stage1-installed: $(host)-stamp-stage1-checked
	cd $(WORKING_DIR) ; $(TIME) $(MAKE) $(MF) "CFLAGS=$(CFLAGS)" install host=$(host)
	cd $(WORKING_DIR) ; $(TIME) $(MAKE) $(MF) "CFLAGS=$(CFLAGS)" install-info host=$(host)
ifeq ($(host),rs6000-ibm-aix)
	rm $(relbindir)/make
endif
	touch $@

$(host)-stamp-stage1-checked: $(host)-stamp-stage1-built
#	cd $(WORKING_DIR) ; $(TIME) $(MAKE) $(MF) "CFLAGS=$(CFLAGS)" check host=$(host)
	touch $@

$(host)-stamp-stage1-built: $(host)-stamp-stage1-configured
	cd $(WORKING_DIR) ; $(TIME) $(MAKE) $(MF) "CFLAGS=$(CFLAGS)" all host=$(host)
	cd $(WORKING_DIR) ; $(TIME) $(MAKE) $(MF) "CFLAGS=$(CFLAGS)" info host=$(host)
	touch $@

$(host)-stamp-stage1-configured:
	[ -d $(WORKING_DIR) ] || mkdir $(WORKING_DIR)
	cd $(WORKING_DIR) ; \
	  $(TIME) ../$(TREE)/configure $(host) -v -srcdir=../$(TREE) $(prefixes)
	touch $@

.PHONY: do2
do2:	$(HOLESDIR) $(host)-stamp-stage2

$(host)-stamp-stage2:
	if [ -d $(STAGE2DIR) ] ; then \
		mv $(STAGE2DIR) $(WORKING_DIR) ; \
	else \
		true ; \
	fi
	PATH=$(relbindir):`pwd`/$(HOLESDIR) ; \
	  export PATH ; \
	  SHELL=sh ; export SHELL ; \
	  $(TIME) $(GNU_MAKE) $(FLAGS_TO_PASS) -f test-build.mk -w $(host)-stamp-stage2-installed
	mv $(WORKING_DIR) $(STAGE2DIR)
	touch $@


$(host)-stamp-stage2-installed: $(host)-stamp-stage2-checked
	cd $(WORKING_DIR) ; $(TIME) $(MAKE) -w $(MF) $(GNUC) "CFLAGS=$(CFLAGS)" install host=$(host)
	cd $(WORKING_DIR) ; $(TIME) $(MAKE) -w $(MF) $(GNUC) "CFLAGS=$(CFLAGS)" install-info host=$(host)
	touch $@

$(host)-stamp-stage2-checked: $(host)-stamp-stage2-built
#	cd $(WORKING_DIR) ; $(TIME) $(MAKE) -w $(MF) $(GNUC) "CFLAGS=$(CFLAGS)" check host=$(host)
	touch $@

$(host)-stamp-stage2-built: $(host)-stamp-stage2-configured
	cd $(WORKING_DIR) ; $(TIME) $(MAKE) -w $(MF) $(GNUC) "CFLAGS=$(CFLAGS)" all host=$(host)
	cd $(WORKING_DIR) ; $(TIME) $(MAKE) -w $(MF) $(GNUC) "CFLAGS=$(CFLAGS)" info host=$(host)
	touch $@

$(host)-stamp-stage2-configured:
	[ -d $(WORKING_DIR) ] || mkdir $(WORKING_DIR)
	cd $(WORKING_DIR) ; \
	  $(TIME) ../$(TREE)/configure $(host) -v -srcdir=../$(TREE) $(prefixes)
	touch $@

.PHONY: do3
do3:	$(HOLESDIR) $(host)-stamp-stage3

$(host)-stamp-stage3:
	if [ -d $(STAGE3DIR) ] ; then \
		mv $(STAGE3DIR) $(WORKING_DIR) ; \
	else \
		true ; \
	fi
	PATH=$(relbindir):`pwd`/$(HOLESDIR) ; \
	  export PATH ; \
	  SHELL=sh ; export SHELL ; \
	  $(TIME) $(GNU_MAKE) $(FLAGS_TO_PASS) -f test-build.mk -w $(host)-stamp-stage3-checked 
	mv $(WORKING_DIR) $(STAGE3DIR) 
	touch $@


$(host)-stamp-stage3-installed: $(host)-stamp-stage3-checked
	cd $(WORKING_DIR) ; $(TIME) $(MAKE) -w $(MF) $(GNUC) "CFLAGS=$(CFLAGS)" install host=$(host)
	cd $(WORKING_DIR) ; $(TIME) $(MAKE) -w $(MF) $(GNUC) "CFLAGS=$(CFLAGS)" install-info host=$(host)
	touch $@

$(host)-stamp-stage3-checked: $(host)-stamp-stage3-built
#	cd $(WORKING_DIR) ; $(TIME) $(MAKE) -w $(MF) $(GNUC) "CFLAGS=$(CFLAGS)" check host=$(host)
	touch $@

$(host)-stamp-stage3-built: $(host)-stamp-stage3-configured
	cd $(WORKING_DIR) ; $(TIME) $(MAKE) -w $(MF) $(GNUC) "CFLAGS=$(CFLAGS)" all host=$(host)
	cd $(WORKING_DIR) ; $(TIME) $(MAKE) -w $(MF) $(GNUC) "CFLAGS=$(CFLAGS)" info host=$(host)
	touch $@

$(host)-stamp-stage3-configured:
	[ -d $(WORKING_DIR) ] || mkdir $(WORKING_DIR)
	cd $(WORKING_DIR) ; \
	  $(TIME) ../$(TREE)/configure $(host) -v -srcdir=../$(TREE) $(prefixes)
	touch $@

# These things are needed by a three-stage, but are not included locally.

HOLES := \
	[ \
	ar \
	as \
	awk \
	basename \
	cat \
	cc \
	chmod \
	cmp \
	cp \
	date \
	diff \
	echo \
	egrep \
	ex \
	expr \
	find \
	grep \
	head \
	hostname \
	install \
	ld \
	lex \
	ln \
	ls \
	make \
	mkdir \
	mv \
	nm \
	pwd \
	ranlib \
	rm \
	rmdir \
	sed \
	sh \
	sort \
	test \
	time \
	touch \
	tr \
	true \
	wc \
	whoami

### so far only sun make supports VPATH
ifeq ($(subst sun3,sun4,$(host)),sun4)
MAKE_HOLE :=
else
MAKE_HOLE := make
endif

### solaris 2 -- don't use /usr/ucb/cc
ifeq (sparc-sun-solaris2,$(host))
PARTIAL_HOLE_DIRS := /opt/cygnus/bin
CC_HOLE	:= cc
else
CC_HOLE :=
endif

### rs6000 as is busted.  We cache a patched version in unsupported.
ifeq ($(host),rs6000-ibm-aix)
AS_HOLE := as
else
AS_HOLE :=
endif

### These things are also needed by a three-stage, but in this case, the GNU version of the tool is required.
PARTIAL_HOLES := \
	$(AS_HOLE) \
	$(MAKE_HOLE) \
	$(CC_HOLE) \
	flex \
	m4

### look in these directories for things missing from a three-stage
HOLE_DIRS := \
	$(HOLE_DIRS) \
	/bin \
	/usr/bin \
	/usr/ucb \
	/usr/ccs/bin \
	/usr/unsupported/bin

### look in these directories for alternate versions of some tools.
PARTIAL_HOLE_DIRS := \
	/usr/latest/bin \
	/usr/progressive/bin \
	$(PARTIAL_HOLE_DIRS) \
	/usr/vintage/bin \
	/usr/unsupported/bin

$(HOLESDIR): $(host)-stamp-holes

$(host)-stamp-holes:
	-rm -rf $(HOLESDIR)
	-mkdir $(HOLESDIR)
	@for i in $(HOLES) ; do \
		found= ; \
		for j in $(HOLE_DIRS) ; do \
			if [ -x $$j/$$i ] ; then \
				ln -s $$j/$$i $(HOLESDIR) || cp $$j/$$i $(HOLESDIR) ; \
				echo $$i from $$j ; \
				found=t ; \
				break ; \
			fi ; \
		done ; \
		case "$$found" in \
		t) ;; \
		*) echo $$i is NOT found ;; \
		esac ; \
	done
	@for i in $(PARTIAL_HOLES) ; do \
		found= ; \
		for j in $(PARTIAL_HOLE_DIRS) ; do \
			if [ -x $$j/$$i ] ; then \
				rm -f $(HOLESDIR)/$$i ; \
				cp $$j/$$i $(HOLESDIR)/$$i || exit 1; \
				echo $$i from $$j ; \
				found=t ; \
				break ; \
			fi ; \
		done ; \
		case "$$found" in \
		t) ;; \
		*) echo $$i is NOT found ;; \
		esac ; \
	done
	touch $@

.PHONY: comparison
comparison: $(host)-stamp-3stage-compared

$(host)-stamp-3stage-compared:
	rm -f .bad-compare
ifeq ($(subst i386-sco3.2v4,mips-sgi-irix4,$(subst rs6000-ibm-aix,mips-sgi-irix4,$(subst mips-dec-ultrix,mips-sgi-irix4,$(host)))),mips-sgi-irix4)
	for i in `cd $(STAGE3DIR) ; find . -name \*.o -print` ; do \
		tail +10c $(STAGE2DIR)/$$i > foo1 ; \
		tail +10c $(STAGE3DIR)/$$i > foo2 ; \
		if cmp foo1 foo2 ; then \
			true ; \
		else \
			echo $$i ; \
			touch .bad-compare ; \
		fi ; \
	done
	rm -f foo1 foo2
else
	for i in `cd $(STAGE3DIR) ; find . -name \*.o -print` ; do \
		cmp $(STAGE2DIR)/$$i $(STAGE3DIR)/$$i || touch .bad-compare ; \
	done
endif
	if [ -f CLEAN_ALL ] ; then \
	  rm -rf $(STAGE2DIR) $(STAGE3DIR) ; \
	else \
	  if [ -f CLEAN_STAGES ] ; then \
	    if [ -f .bad-compare ] ; then \
	      true ; \
	    else \
	      rm -rf $(STAGE1DIR) $(STAGE2DIR) ; \
	    fi ; \
	  else true ; \
	  fi ; \
	fi
	touch $@	

.PHONY: clean
clean:
	rm -rf $(HOLESDIR) $(INPLACEDIR) $(WORKING_DIR)* $(host)-stamp-* *~

.PHONY: very
very:
	rm -rf $(TREE)

force:

endif # host

### Local Variables:
### fill-column: 131
### End:
