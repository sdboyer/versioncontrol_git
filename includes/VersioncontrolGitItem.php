<?php

class VersioncontrolGitItem extends VersioncontrolItem {

  /**
   * Implementation abstract method.
   */
  public function getSelectedLabelFromItem(&$other_item, $other_item_tags = array()) {
    // First up, optimizations - maybe we can do without the generic
    // "label transfer" code from further down and use assumptions instead.
    // Let's assume for FakeVCS repositories that if an item wears a label, then
    // an item at another path but with the same (file-level) revision can also
    // wear that same label. That is the case with some version control systems
    // (e.g. Git, Mercurial, Bazaar) but might not be the case with others
    // (CVS for its lack of global revision identifiers, SVN for its use of
    // directory structure as tag/branch identifiers).
    if ($this->revision == $other_item->revision) {
      return $other_item->getSelectedLabel();
    }

    //can be maybe optimized for speed by using the hints provided
    return _versioncontrol_git_get_branch_intersect($this->repository, $this, $other_item);
  }

}
