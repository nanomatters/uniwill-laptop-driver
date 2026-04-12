.PHONY: all clean cachyos

CFLAGS_uniwill-acpi.o := -DDEBUG
CFLAGS_uniwill-wmi.o := -DDEBUG
obj-m += uniwill-laptop.o
uniwill-laptop-y := uniwill-acpi.o uniwill-wmi.o

PACKAGE_NAME := uniwill-laptop-driver
PACKAGE_VERSION := 0.1.0

all:
	make -C /lib/modules/`uname -r`/build M=`pwd` modules

clean:
	make -C /lib/modules/`uname -r`/build M=`pwd` clean
	rm -f dkms.conf

cachyos:
	@echo "Preparing CachyOS package (using archpkg/PKGBUILD.cachyos)..."
	sed 's/#MODULE_VERSION#/$(PACKAGE_VERSION)/' dkms.conf.in > dkms.conf
	echo 'MAKE[0]="make LLVM=1 -C $${kernel_source_dir} M=$${dkms_tree}/$${PACKAGE_NAME}/$${PACKAGE_VERSION}/build"' >> dkms.conf
	@mkdir -p dist
	@cp archpkg/PKGBUILD.cachyos dist/PKGBUILD
	@cp archpkg/uniwill-laptop-driver.install dist/
	tar -cJf dist/$(PACKAGE_NAME)-$(PACKAGE_VERSION).tar.xz \
		--transform="s,^\./,$(PACKAGE_NAME)-$(PACKAGE_VERSION)/," \
		--exclude='*.cmd' --exclude='*.ko' --exclude='*.mod' --exclude='*.mod.c' \
		--exclude='*.o' --exclude='*.o.d' --exclude='modules.order' \
		--exclude='Module.symvers' --exclude='.git' --exclude='archpkg' \
		--exclude='dist' --exclude='dkms.conf.in' --exclude='.gitignore' \
		-C . uniwill-acpi.c uniwill-wmi.c uniwill-wmi.h Makefile LICENSE dkms.conf
	@cd dist && (makepkg --cleanbuild -s --noconfirm || \
		(echo "makepkg failed, retrying without dependency installation..." && \
		makepkg --cleanbuild --nodeps --noconfirm)) || \
		echo "makepkg not available or failed; PKGBUILD and tarball are in dist/"
