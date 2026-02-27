# -*-Makefile-*-

OBJDIRS		+= $(USER_OBJDIR)/sigsegv_test

USER_SIGSEGV_SRC	+= $(USER_DIR)/sigsegv_test/sigsegv_test.c
USER_SIGSEGV_OBJ	:= $(patsubst %.c, $(OBJDIR)/%.o, $(USER_SIGSEGV_SRC))
USER_SIGSEGV_OBJ	:= $(patsubst %.S, $(OBJDIR)/%.o, $(USER_SIGSEGV_OBJ))
KERN_BINFILES	+= $(USER_OBJDIR)/sigsegv_test/sigsegv_test

sigsegv_test: $(USER_OBJDIR)/sigsegv_test/sigsegv_test

$(USER_OBJDIR)/sigsegv_test/sigsegv_test: $(USER_LIB_OBJ) $(USER_SIGSEGV_OBJ)
	@echo + ld[USER/sigsegv_test] $@
	$(V)$(LD) -o $@ $(USER_LDFLAGS) $(USER_LIB_OBJ) $(USER_SIGSEGV_OBJ) $(GCC_LIBS)
	$(V)$(OBJDUMP) -S $@ > $@.asm
	$(V)$(NM) -n $@ > $@.sym

$(USER_OBJDIR)/sigsegv_test/%.o: $(USER_DIR)/sigsegv_test/%.c
	@echo + cc[USER/sigsegv_test] $<
	@mkdir -p $(@D)
	$(V)$(CC) $(USER_CFLAGS) -c -o $@ $<

$(USER_OBJDIR)/sigsegv_test/%.o: $(USER_DIR)/sigsegv_test/%.S
	@echo + as[USER/sigsegv_test] $<
	@mkdir -p $(@D)
	$(V)$(CC) $(USER_CFLAGS) -c -o $@ $<
