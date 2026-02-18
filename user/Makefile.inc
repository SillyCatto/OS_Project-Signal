# -*-Makefile-*-

USER_DIR	:= user
USER_OBJDIR	:= $(OBJDIR)/user

OBJDIRS		+= $(USER_OBJDIR)

include $(USER_DIR)/config.mk

USER_CFLAGS	+= $(CFLAGS) -m32 -D_USER_ -O2 -g -I$(USER_DIR)/include -I$(KERN_DIR)
USER_LDFLAGS	:= $(LDFLAGS) -m elf_i386 -Ttext=0x40000000 -e _start

include $(USER_DIR)/lib/Makefile.inc
include $(USER_DIR)/idle/Makefile.inc
include $(USER_DIR)/pingpong/Makefile.inc
include $(USER_DIR)/fstest/Makefile.inc
include $(USER_DIR)/shell/Makefile.inc

# Signal test program
SIGNAL_TEST_SRC	:= $(USER_DIR)/signal_test.c
SIGNAL_TEST_OBJ	:= $(USER_OBJDIR)/signal_test.o
SIGNAL_TEST_BIN	:= $(USER_OBJDIR)/signal_test

$(SIGNAL_TEST_OBJ): $(SIGNAL_TEST_SRC)
	@echo + cc $<
	$(V)$(CC) -nostdinc $(USER_CFLAGS) -c -o $@ $<

$(SIGNAL_TEST_BIN): $(SIGNAL_TEST_OBJ) $(USER_OBJDIR)/lib/libuser.a
	@echo + ld $@
	$(V)$(LD) $(USER_LDFLAGS) -o $@ $^

user: lib idle pingpong fstest $(SIGNAL_TEST_BIN)
	@echo All targets of user are done.

install_user:
	$(V)cp $(SIGNAL_TEST_BIN) $(OBJDIR)/user/
