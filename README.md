# linux-dreambox-meson64

Kernel sources for the Dreambox One and Dreambox Two (Amlogic S922X, meson64)
as built by the oe-alliance images.

`master` only holds this documentation. Each kernel version lives in its own
branch:

| Branch    | Kernel | Recipe |
|-----------|--------|--------|
| `4.9.113` | DMM linux-meson64 4.9.113-643176-g72b0573 | `meta-brands/meta-dream/recipes-linux/linux-meson64_4.9.113-643176-g72b0573.bb` in [oe-alliance-core](https://github.com/oe-alliance/oe-alliance-core) |

## Branch layout

Every branch starts from the public Amlogic tree this repository is forked
from ([CoreELEC/linux-amlogic](https://github.com/CoreELEC/linux-amlogic)) and
keeps the history on top of it readable:

1. The closest public Amlogic commit the vendor kernel is based on.
2. One commit with the vendor's source drop, unchanged.
3. One commit per patch the recipe applies, in recipe order, with the author
   who added the patch to oe-alliance-core.
4. The source fixes the recipe applies without a patch file
   (`kernel-fixups.bbclass`, `do_configure` edits).

The tree at the tip of a branch is the source the recipe builds.

### 4.9.113

- Base: `485ae37b252`, the last Amlogic 4.9.113 commit before Amlogic moved
  to 4.9.180. DMM's own commit `g72b0573` is not public.
- `DMM linux-meson64 4.9.113-643176-g72b0573`: the archive the recipe fetches
  from source.mynonpublic.com. Besides DMM's changes it holds the parts DMM
  backported from the newer Amlogic tree.
- The 15 recipe patches, `kernel-fixups.bbclass` and the `SUBLEVEL` drop of
  `linux-dreambox-4.9.inc`, which makes the release `4.9` that DMM's modules
  are built for.
