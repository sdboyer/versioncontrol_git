<?php

class VersioncontrolGitOperation extends VersioncontrolOperation {

  /**
   * Implementation of abstract method.
   */
  public function getSelectedLabel($target_item) {
  // TODO: implement tag support here, tags>branch?
  // better not, after looking it again current code is OK.
  // just take the first branch, dunno what else we should do here...
  // jpetso knows neither :P
    return $this->labels[0];
  }

}