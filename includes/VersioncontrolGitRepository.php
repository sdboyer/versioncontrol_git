<?php

class VersioncontrolGitRepository extends VersioncontrolRepository {

  /**
   * Overwrite to get short sha-1's
   */
  public function formatRevisionIdentifier($revision, $format = 'full') {
    switch ($format) {
    case 'short':
      // Let's return only the first 7 characters of the revision identifier,
      // like git log --abbrev-commit does by default.
      return substr($revision, 0, 7);
    case 'full':
    default:
      return $revision;
    }
  }

}
