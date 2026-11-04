# io：ILoader、StepLoader、IgesLoader、ProgressStreamBuf、OcctProgressBridge（T05）
steplens_add_module(io Anchor.cpp OcctExceptions.cpp OcctExceptions.h occt_compat.h)
steplens_link_occt(steplens_io)
