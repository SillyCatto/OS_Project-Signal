# -*-Makefile-*-

OBJDIRS		+= $(USER_OBJDIR)/sigint_custom_test

USER_SIGINT_CUSTOM_SRC	+= $(USER_DIR)/sigint_custom_test/sigint_custom_test.c
USER_SIGINT_CUSTOM_OBJ	:= $(patsubst %.c, $(OBJDIR)/%.o, $(USER_SIGINT_CUSTOM_SRC))
USER_SIGINT_CUSTOM_OBJ	:= $(patsubst %.S, $(OBJDIR)/%.o, $(USER_SIGINT_CUSTOM_OBJ))
KERN_BINFILES	+= $(USER_OBJDIR)/sigint_custom_test/sigint_custom_test

sigint_custom_test: $(USER_OBJDIR)/sigint_custom_test/sigint_custom_test

$(USER_OBJDIR)/sigint_custom_test/sigint_custom_test: $(USER_LIB_OBJ) $(USER_SIGINT_CUSTOM_OBJ)
	@echo + ld[USER/sigint_custom_test] $@
	$(V)$(LD) -o $@ $(USER_LDFLAGS) $(USER_LIB_OBJ) $(USER_SIGINT_CUSTOM_OBJ) $(GCC_LIBS)
	$(V)$(OBJDUMP) -S $@ > $@.asm
	$(V)$(NM) -n $@ > $@.sym

$(USER_OBJDIR)/sigint_custom_test/%.o: $(USER_DIR)/sigint_custom_test/%.c
	@echo + cc[USER/sigint_custom_test] $<
	@mkdir -p $(@D)
	$(V)$(CC) $(USER_CFLAGS) -c -o $@ $<

$(USER_OBJDIR)/sigint_custom_test/%.o: $(USER_DIR)/sigint_custom_test/%.S
	@echo + as[USER/sigint_custom_test] $<
	@mkdir -p $(@D)
	$(V)$(CC) $(USER_CFLAGS) -c -o $@ $<
