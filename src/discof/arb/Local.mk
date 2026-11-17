$(call add-objs,fd_arbfilter,fd_discof)
$(call add-hdrs,fd_arbfilter.h fd_arbnotify.h)
ifdef FD_HAS_HOSTED
$(call make-unit-test,test_arbfilter,test_arbfilter,fd_discof fd_ballet fd_util)
$(call run-unit-test,test_arbfilter)
endif
