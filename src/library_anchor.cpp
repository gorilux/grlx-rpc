// Anchor translation unit for the grlx-rpc library.
//
// grlx-rpc is header-only, so this target has no sources of its own. Meson
// warns on a build target with an empty source list and is dropping support
// for it, so keep one always-compiled TU here.

namespace grlx::rpc::detail {

// Deliberately unreferenced: present only to give the library a TU.
void grlx_rpc_anchor() {
}

} // namespace grlx::rpc::detail
