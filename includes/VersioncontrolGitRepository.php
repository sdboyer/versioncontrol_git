<?php

class VersioncontrolGitRepository extends VersioncontrolRepository {

  /**
   * State flag indicating whether or not the GIT_DIR variable has been pushed
   * into the environment.
   *
   * Used to prevent multiple unnecessary calls to putenv(). Will be obsoleted
   * by the introduction of a cligit library.
   *
   * @var bool
   */
  public $envSet = FALSE;

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

  /**
   * Ensure environment variables are set for interaction with the repository on
   * disk.
   *
   * Hopefully temporary, until we can get a proper cligit library written.
   */
  public function setEnv() {
    if (!$this->envSet) {
      $root = escapeshellcmd($this->root);
      putenv("GIT_DIR=$root/.git");
      $this->envSet = TRUE;
    }
  }

  /**
   * Invoke git to fetch a list of local branches in the repository, including
   * the SHA1 of the current branch tip and the branch name.
   */
  public function fetchBranches() {
    $branches = array();

    $data = array(
      'repo_id' => $this->repo_id,
      'action' => VERSIONCONTROL_ACTION_MODIFIED,
      'label_id' => NULL,
    );
    $logs = $this->exec('git show-ref --heads');
    while (($branchdata = next($logs) !== FALSE)) {
      list($data['tip'], $data['name']) = explode(' ', trim($branchdata));
      $data['name'] = substr($data['name'], 11);
      if (!empty($data['name'])) { // null chars can sneak in, causing empties.
        $branches[$data['name']] = new VersioncontrolBranch($this->backend);
        $branches[$data['name']]->build($data);
      }
    }

    return $branches;
  }

  /**
   * Execute a Git command using the root context and the command to be
   * executed.
   *
   * @param string $command Command to execute.
   * @return mixed Logged output from the command; an array of either strings or
   *  file pointers.
   */
  protected function exec($cmds) {
    if (!$this->envSet) {
      $this->setEnv();
    }
    $logs = array();
    exec($cmds, $logs);
    array_unshift($logs, '');
    reset($logs); // Reset the array pointer, so that we can use next().
    return $logs;
  }
}
