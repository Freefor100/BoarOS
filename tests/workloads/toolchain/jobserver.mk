.PHONY: all recursive token1 token2 token3 token4 token5 token6
all: recursive
recursive:
	+$(MAKE) -f /inputs/jobserver.mk token1 token2 token3 token4 token5 token6

token1 token2 token3 token4 token5 token6:
	@echo JOBSERVER $(MAKEFLAGS)
	@/tmp/job
