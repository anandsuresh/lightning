#! /usr/bin/make

WIRE_HEADERS :=	wire/onion_defs.h		\
		wire/peer_wire.h		\
		wire/onion_wire.h		\
		wire/tlvstream.h		\
		wire/wire.h			\
		wire/wire_sync.h		\
		wire/wire_io.h			\
		wire/peer$(EXP)_wiregen.h	\
		wire/onion$(EXP)_wiregen.h	\
		wire/bolt12$(EXP)_wiregen.h	\
		wire/common_wiregen.h		\
		wire/channel_type_wiregen.h	\
		wire/peer$(EXP)_printgen.h	\
		wire/onion$(EXP)_printgen.h

# We don't include peer_printgen/onion_printgen here since most don't need it.
WIRE_SRC := wire/wire_sync.c			\
		wire/wire_io.c			\
		wire/fromwire.c			\
		wire/peer_wire.c		\
		wire/tlvstream.c		\
		wire/towire.c			\
		wire/common_wiregen.c		\
		wire/bolt12$(EXP)_wiregen.c	\
		wire/peer$(EXP)_wiregen.c	\
		wire/channel_type_wiregen.c	\
		wire/onion$(EXP)_wiregen.c	

WIRE_PRINT_SRC :=				\
		wire/onion$(EXP)_printgen.c	\
		wire/peer$(EXP)_printgen.c	\
		wire/channel_type_printgen.c

WIRE_OBJS := $(WIRE_SRC:.c=.o)
WIRE_PRINT_OBJS := $(WIRE_PRINT_SRC:.c=.o)
$(WIRE_OBJS) $(WIRE_PRINT_OBJS): $(WIRE_HEADERS)

# Make sure these depend on everything: in case we're experimental,
# include non-experimental ones here so they get rebuilt.
WIRE_NONEXP_SRC := wire/bolt12_wiregen.c	\
		wire/peer_wiregen.c		\
		wire/onion_wiregen.c		\
		wire/onion_printgen.c		\
		wire/peer_printgen.c
WIRE_NONEXP_HEADERS := wire/peer_wiregen.h	\
		wire/onion_wiregen.h		\
		wire/bolt12_wiregen.h		\
		wire/peer_printgen.h		\
		wire/onion_printgen.h		\
		wire/channel_type_printgen.h

ALL_C_SOURCES += $(WIRE_SRC) $(WIRE_PRINT_SRC) $(WIRE_NONEXP_SRC)

ALL_C_HEADERS += $(WIRE_HEADERS) $(WIRE_NONEXP_HEADERS)

# They may not have the bolts.
BOLT_EXTRACT=$(LOCAL_BOLTDIR)/tools/extract-formats.py

WIRE_BOLT_DEPS := $(BOLT_DEPS) tools/gen/impl_template tools/gen/header_template

ALL_PEER_PATCHES := $(sort $(wildcard wire/extracted_peer*.patch))
ALL_ONION_PATCHES := $(sort $(wildcard wire/extracted_onion*.patch))
ALL_BOLT12_PATCHES := $(sort $(wildcard wire/extracted_bolt12*.patch))

# These are applied to the non-exp csvs to make the exp csvs.
PEER_EXP_PATCHES := $(sort $(wildcard wire/extracted_peer_exp*.patch))
ONION_EXP_PATCHES := $(sort $(wildcard wire/extracted_onion_exp*.patch))
BOLT12_EXP_PATCHES := $(sort $(wildcard wire/extracted_bolt12_exp*.patch))

# These are always applied to the bolts.
PEER_PATCHES := $(filter-out $(PEER_EXP_PATCHES),$(ALL_PEER_PATCHES))
ONION_PATCHES := $(filter-out $(ONION_EXP_PATCHES),$(ALL_ONION_PATCHES))
BOLT12_PATCHES := $(filter-out $(BOLT12_EXP_PATCHES),$(ALL_BOLT12_PATCHES))

# Explicit command to re-extract CSV from BOLTs and patch.
# This is not a normal make depencency, since we don't want this
# called implicitly.
# Note: You will need to run extract-bolt12-csv manually!
extract-bolt-csv: extract-peer-csv extract-onion-csv

extract-peer-csv: wire/peer_wire.csv.raw
	@set -e; T=wire/peer_wire.csv; trap "rm -f $$T.$$$$" 0; cp $< $$T.$$$$; for p in $(PEER_PATCHES); do echo APPLY $$p; patch $$T.$$$$ $$p; done; mv $$T.$$$$ $$T

extract-onion-csv: wire/onion_wire.csv.raw
	@set -e; T=wire/onion_wire.csv; trap "rm -f $$T.$$$$" 0; cp $< $$T.$$$$; for p in $(ONION_PATCHES); do echo APPLY $$p; patch $$T.$$$$ $$p; done; mv $$T.$$$$ $$T

extract-bolt12-csv: wire/bolt12_wire.csv.raw
	@set -e; T=wire/bolt12_wire.csv; trap "rm -f $$T.$$$$" 0; cp $< $$T.$$$$; for p in $(BOLT12_PATCHES); do echo APPLY $$p; patch $$T.$$$$ $$p; done; mv $$T.$$$$ $$T

wire/peer_wire.csv.raw: bolt-precheck
	@$(BOLT_EXTRACT) $(LOCAL_BOLTDIR)/0[127]*.md > $@

wire/onion_wire.csv.raw: bolt-precheck
	@(echo '#include <wire/onion_defs.h>'; $(BOLT_EXTRACT) $(LOCAL_BOLTDIR)/04*.md) > $@

wire/bolt12_wire.csv.raw: bolt-precheck
	@$(BOLT_EXTRACT) $(LOCAL_BOLTDIR)/12*.md > $@

# How to make the exp versions from non-exp versions:
wire/peer_exp_wire.csv: wire/peer_wire.csv $(PEER_EXP_PATCHES)
	@set -e; T=$@; trap "rm -f $$T.$$$$" 0; cp $< $$T.$$$$; for p in $(PEER_EXP_PATCHES); do echo APPLY $$p; patch $$T.$$$$ $$p; done; mv $$T.$$$$ $$T

wire/onion_exp_wire.csv: wire/onion_wire.csv $(ONION_EXP_PATCHES)
	@set -e; T=$@; trap "rm -f $$T.$$$$" 0; cp $< $$T.$$$$; for p in $(ONION_EXP_PATCHES); do echo APPLY $$p; patch $$T.$$$$ $$p; done; mv $$T.$$$$ $$T

wire/bolt12_exp_wire.csv: wire/bolt12_wire.csv $(BOLT12_EXP_PATCHES)
	@set -e; T=$@; trap "rm -f $$T.$$$$" 0; cp $< $$T.$$$$; for p in $(BOLT12_EXP_PATCHES); do echo APPLY $$p; patch $$T.$$$$ $$p; done; mv $$T.$$$$ $$T

# These can be deleted.
.INTERMEDIATE: wire/peer_wire.csv.raw wire/onion_wire.csv.raw wire/bolt12_wire.csv.raw

# Explicit command to add patchfile for BOLT CSV's against current one.
generate-bolt-csv-patch: bolt-precheck
	@$(BOLT_EXTRACT) $(LOCAL_BOLTDIR)/0[127]*.md | diff -u wire/peer$(EXP)_wire.csv - >wire/extracted_peer$(EXP)_$(BOLTVERSION).patch | if [ $$? -lt 0 ];then exit 1;fi; echo wire/extracted_peer$(EXP)_$(BOLTVERSION).patch
	@{ echo '#include <wire/onion_defs.h>'; $(BOLT_EXTRACT) $(LOCAL_BOLTDIR)/04*.md; } | diff -u wire/onion$(EXP)_wire.csv - > wire/extracted_onion$(EXP)_$(BOLTVERSION).patch | if [ $$? -lt 0 ];then exit 1;fi; echo wire/extracted_onion$(EXP)_$(BOLTVERSION).patch

# tlvs_n1 and n2 are used for test vectors, thus not referenced: expose them
# for testing and to prevent compile error about them being unused.
# This will be easier if test vectors are moved to separate files.
wire/peer_wiregen.h_args := --include='common/channel_id.h' --include='bitcoin/tx.h' --include='bitcoin/preimage.h' --include='bitcoin/short_channel_id.h' --include='common/node_id.h' --include='common/bigsize.h' --include='bitcoin/block.h' --include='bitcoin/privkey.h' -s --expose-tlv-type=n1 --expose-tlv-type=n2

wire/peer_wiregen.c_args := -s --expose-tlv-type=n1 --expose-tlv-type=n2

# The tlv_payload isn't parsed in a fromwire, so we need to expose it.
wire/onion_wiregen.h_args := --include='bitcoin/short_channel_id.h' --include='bitcoin/privkey.h' --include='common/bigsize.h' --include='common/amount.h' --include='common/node_id.h' --include='bitcoin/block.h' -s --expose-tlv-type=tlv_payload

wire/onion_wiregen.c_args := -s --expose-tlv-type=tlv_payload

# Same for _exp versions
wire/peer_exp_wiregen.h_args := $(wire/peer_wiregen.h_args) --include='wire/channel_type_wiregen.h'
wire/peer_exp_wiregen.c_args := $(wire/peer_wiregen.c_args)
wire/peer_exp_printgen.h_args := --include='wire/channel_type_printgen.h'
wire/onion_exp_wiregen.h_args := $(wire/onion_wiregen.h_args)
wire/onion_exp_wiregen.c_args := $(wire/onion_wiregen.c_args)

wire/bolt12_wiregen.c_args := -s --expose-tlv-type=blinded_path --expose-tlv-type=invoice_request
wire/bolt12_wiregen.h_args := --include='bitcoin/short_channel_id.h' --include='bitcoin/signature.h' --include='bitcoin/privkey.h' --include='common/bigsize.h' --include='common/amount.h' --include='common/node_id.h' --include='bitcoin/block.h'  --include='wire/onion_wire.h' $(wire/bolt12_wiregen.c_args)
# Same for _exp versions
wire/bolt12_exp_wiregen.h_args := $(wire/bolt12_wiregen.h_args)
wire/bolt12_exp_wiregen.c_args := $(wire/bolt12_wiregen.c_args)

wire/peer_wiregen.h_args := --include='common/channel_id.h' --include='bitcoin/tx.h' --include='bitcoin/preimage.h' --include='bitcoin/short_channel_id.h' --include='common/node_id.h' --include='common/bigsize.h' --include='bitcoin/block.h' --include='bitcoin/privkey.h' -s --expose-tlv-type=n1 --expose-tlv-type=n2

wire/channel_type_wiregen.h_args := -s
wire/channel_type_wiregen.c_args := $(wire/channel_type_wiregen.h_args)

# All generated wire/ files depend on this Makefile
$(filter %printgen.h %printgen.c %wiregen.h %wiregen.c, $(WIRE_SRC) $(WIRE_PRINT_SRC) $(WIRE_NONEXP_SRC) $(WIRE_HEADERS) $(WIRE_NONEXP_HEADERS)): wire/Makefile

maintainer-clean: wire-maintainer-clean

clean: wire-clean

wire-clean:
	$(RM) wire/*.csv.*
	$(RM) wire/channel_type_*gen*

wire-maintainer-clean: wire-clean
	$(RM) wire/gen_*_csv wire/extracted_*_experimental_csv

include wire/test/Makefile
