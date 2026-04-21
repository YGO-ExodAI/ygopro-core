# ExodAI Phase P1 Primitive 1 — protobuf schema codegen (dev helper only).
#
# BOTH build systems (xmake for ygoenv, premake+MSBuild for EDOPro) now
# generate .pb.cc/.pb.h at build time from .proto, using their own protoc
# binary matching their own libprotobuf runtime. Generated files are
# .gitignore'd. This helper is useful for:
#
#   - Quickly validating .proto syntax during local iteration.
#   - Populating generated files for IDE completion / syntax highlighting.
#   - Inspecting a reference .pb.h without kicking off a full build.
#
# `make check` is NOT a CI gate anymore (there's no canonical committed
# gencode to compare against). The CI gates live in the build systems.
#
# Reasoning for the pivot to build-time generation: protobuf's gencode
# enforces a strict `PROTOBUF_VERSION != N` check against the runtime it
# will link to. xmake-repo and vcpkg's protobuf catalogs don't have an
# overlapping version, so any single committed .pb.cc would only be
# loadable by one side. See phase_p1_primitive_1_plan.md §1.3.

PROTOC ?= xrepo env -b "protobuf-cpp 28.x" protoc
PROTO_SRC := ocg_state.proto
PROTO_GEN := ocg_state.pb.cc ocg_state.pb.h

.PHONY: proto clean

proto: $(PROTO_GEN)

$(PROTO_GEN): $(PROTO_SRC)
	$(PROTOC) --proto_path=. --cpp_out=. $(PROTO_SRC)
	@echo "[make proto] regenerated $(PROTO_GEN) locally (not committed)"

clean:
	rm -f $(PROTO_GEN)
