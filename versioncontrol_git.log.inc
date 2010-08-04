<?php
// $Id$
/**
 * @file
 * Git backend for Version Control API - Provides Git commit information and
 * account management as a pluggable backend.
 *
 * Copyright 2008 by Jimmy Berry ("boombatower", http://drupal.org/user/214218)
 * Copyright 2009-2010 by Cornelius Riemenschneider ("CorniI", http://drupal.org/user/136353)
 */

/**
 * Actually update the repository by fetching commits and other stuff
 * directly from the repository, invoking the git executable.
 * @param VersioncontrolGitRepository $repository
 * @return
 *   TRUE if the logs were updated, or FALSE if fetching and updating the logs
 *   failed for whatever reason.
 */
function _versioncontrol_git_log_update_repository(&$repository) {
  $root = escapeshellcmd($repository->root);
  putenv("GIT_DIR=$root/.git");
  if ($repository->data['versioncontrol_git']['locked'] == TRUE) {
    drupal_set_message(t('This repository is locked, there is already a fetch in progress. If this is not the case, press the clear lock button.'), 'error');
    return FALSE;
  }
  // $repository->data['versioncontrol_git']['locked'] = 1;
  // $repository->update();

  $branches_in_repo = $repository->fetchBranches();
  $branches_in_db = $repository->loadBranches(array(), array(), array('idkey' => 'name'));
  $branches_new = array_diff_key($branches_in_repo, $branches_in_db);
  $branches_deleted = array_diff_key($branches_in_db, $branches_in_repo);

  // Insert new branches in the repository. Later all commits in these new
  // branches will be updated.
  // Unfortunately we can't say anything about the branch author at this time.
  // The post-update hook could do this, though.
  // We also can't insert a VCOperation which adds the branch, because
  // we don't know anything about the branch. This is all stuff a hook could
  // figure out.
  // Here we will just ensure that each branch is in the database.
  foreach($branches_new as $branch) {
    $branch->ensure();
  }

  // Deleted branches are removed, commits in them are not!
  // TODO: the db_query in here is not nice, that should be part of vcapi!
  foreach($branches_deleted as $branch_name) {
    _versioncontrol_git_log_remove_branch_from_commits($repository, $branch_name);
    db_query('DELETE FROM {versioncontrol_labels}
              WHERE repo_id = %d AND type = %d AND label_id = %d',
              $repository->repo_id, VERSIONCONTROL_LABEL_BRANCH,
              $branches_in_db[$branch_name]->label_id);
  }

  $commits_in_db = _versioncontrol_git_log_get_commits_in_db($repository);
  $commits_in_repo = _versioncontrol_git_log_get_commits_in_repo($repository);
  $commits_new = array_diff($commits_in_repo, $commits_in_db);

  // Insert new commits in the database.
  foreach($commits_new as $new_commit) {
    $new_commit_data = _versioncontrol_git_log_get_raw_commit_data($new_commit);
    _versioncontrol_git_log_parse_and_insert_commit($repository, $new_commit_data, $commits_in_db, $branches_in_db);
  }

  // Add a new branch to all commits contained in that branch.
  foreach($branches_new as $branch_new) {
  	$commits = _versioncontrol_git_log_get_commits_in_branch($branch_new);
    _versioncontrol_git_log_attach_branch_to_commits($branches_in_db[$branch_new], $commits);
  }

  // Insert new tags in the database.
  $tags_in_repo = _versioncontrol_git_log_get_tags_in_repo();
  $tags_in_db = _versioncontrol_git_log_get_tags_in_db($repository);
  // Deleting tags is *not* supported. Read the manual if you want to know why...
  // Check for new tags.
  $tags_new = array_diff($tags_in_repo, $tags_in_db);
  if (!empty($tags_new)) {
    _versioncontrol_git_log_process_tags($repository, $tags_new);
  }

  // Update repository updated field. Displayed on administration interface for documentation purposes.
  $repository->data['versioncontrol_git']['updated'] = time();
  $repository->data['versioncontrol_git']['locked'] = 0;
  $repository->update();
  return TRUE;
}

/// All general functions

/**
 * Execute a Git command using the root context and the command to be executed.
 * @param string $command Command to execute.
 * @return mixed Logged output from the command in either array of file pointer form.
 */
function _versioncontrol_git_log_exec($command) {
  $logs = array();
  exec($command, $logs);
  array_unshift($logs, '');
  reset($logs); // Reset the array pointer, so that we can use next().
  return $logs;
}

/**
 * Returns a list of all existing labels in the database of $type.
 * @param VersioncontrolRepository $repository
 * @param $type
 * @return array An array of branch/tag names.
 */
function _versioncontrol_git_log_get_labels_in_db($repository, $type) {
  $labels_in_db = $repository->getLabels(array('type' => $type));
  $label_names_in_db = array();
  foreach($labels_in_db as $label) {
  	$label_names_in_db[] = $label->name;
  }
  return $label_names_in_db;
}

/// All commit related function

/**
 * Return one commit op.
 * @param VersioncontrolRepository $repository
 * @param string $revision
 * @return VersioncontrolOperation
 */
function _versioncontrol_git_log_get_commit($repository, $revision) {
  $constraints = array(
    'repo_ids' => array($repository->repo_id),
    'types' => array(VERSIONCONTROL_OPERATION_COMMIT),
    'revisions' => array($revision),
  );
  $commit_op = VersioncontrolOperationCache::getInstance()->getOperations($constraints);
  return array_pop($commit_op);
}

/**
 * Returns a list of all existing commits in the database.
 * @param VersioncontrolRepository $repository
 * @return array An array of commit revisions.
 */
function _versioncontrol_git_log_get_commits_in_db($repository) {
  $constraints = array(
    'vcs' => array('git'),
    'repo_ids' => array($repository->repo_id),
    'types' => array(VERSIONCONTROL_OPERATION_COMMIT),
  );
  $commits_in_db_as_op = VersioncontrolOperationCache::getInstance()->getOperations($constraints);
  $commits_in_db = array();
  foreach($commits_in_db_as_op as $vc_op_id => $vc_op) {
    $commits_in_db[] = $vc_op->revision;
  }
  return $commits_in_db;
}

/**
 * Get the raw git output for $revision.
 * @param string $revision
 * @return The raw git output which can be fed into the commit parser for a given commit.
 */
function _versioncontrol_git_log_get_raw_commit_data($revision) {
  $revision = escapeshellarg($revision);
  $command = "git log $revision --numstat --summary --pretty=format:\"%H%n%P%n%aN <%ae>%n%ct%n%s%n%b%nENDOFOUTPUTGITMESSAGEHERE\" -n 1 -c --";
  return _versioncontrol_git_log_exec($command);
}

/**
 * Returns an array of all branches a given commit is in.
 * @param string $revision
 * @param array $branch_label_list
 * @return VersioncontrolBranch
 */
function _versioncontrol_git_log_get_branches_of_commit($revision, $branch_label_list) {
  $exec = 'git branch --contains ' . escapeshellarg($revision);
  $logs = _versioncontrol_git_log_exec($exec);
  $branches = array();
  while (($line = next($logs)) !== FALSE) {
    $line = trim($line);
    if($line[0] == '*') {
      $line = substr($line, 2);
    }
    $branches[] = $branch_label_list[$line];
  }
  return $branches;
}

/**
 * This function returns all commits in the repository
 * @param $repository
 * @return array An array of strings with all commit id's in it
 */
function _versioncontrol_git_log_get_commits_in_repo($repository) {
  $logs = _versioncontrol_git_log_exec("git rev-list --all");
  $commits = array();
  while (($line = next($logs)) !== FALSE) {
    $commits[] = trim($line);
  }
  return $commits;
}

/**
 * This function returns all commits in the given branch.
 * @param string $branch
 * @return array An array of strings with all commit id's in it
 */
function _versioncontrol_git_log_get_commits_in_branch($branch) {
  $logs = _versioncontrol_git_log_exec("git rev-list " . escapeshellarg($branch) . " --");
  $commits = array();
  while (($line = next($logs)) !== FALSE) {
    $commits[] = trim($line);
  }
  return $commits;
}

/**
 * Returns the previous commits which touched a given file.
 * @param string $path
 * @param string $revision_current
 * @param int $parent_count
 * @return string
 */
function _versioncontrol_git_get_prev_commits_touching_file($path, $revision_current, $parent_count) {
  // We need one more revision to fulfill git rev-list's requirements.
  $parent_count += 1;
  $exec = 'git rev-list -n ' . $parent_count . ' ' . escapeshellarg($revision_current) . ' -- ' . escapeshellarg($path);
  return _versioncontrol_git_log_exec($exec);
}

/**
 * A function to fill in the source_item for a specific VersioncontrolItem.
 * @param VersioncontrolItem $item
 * @param array $parents The parent commit(s)
 * @return none
 */
function _versioncontrol_git_fill_source_item($item, $parents) {
  $parent_count = count($parents);
  $path_stripped = substr($item->path, 1);
  $prev_revisions = _versioncontrol_git_get_prev_commits_touching_file($path_stripped, $item->revision, $parent_count);

  for($i = 0; $i < $parent_count; ++$i) {
    $revision = trim($prev_revisions[$i + 2]);
    $item->source_items[] = new VersioncontrolGitItem(
      VERSIONCONTROL_ITEM_FILE,
      $item->path,
      $revision,
      null,
      $item->repository
    );
  }
}

/**
 * Takes parts of the output of git log and returns all affected OperationItems for a commit.
 * @param VersioncontrolRepository $repository
 * @param array $logs
 * @param string $line
 * @param string $revision
 * @param array $parents The parent commit(s)
 * @param bool $merge
 * @return array All items affected by a commit.
 */
function _versioncontrol_git_parse_items($repository, &$logs, &$line, $revision, $parents, $merge) {
  $op_items = array();
  // Parse the diffstat for the changed files.
  do {
    if (!preg_match('/^(\S+)' . "\t" . '(\S+)' . "\t" . '(.+)$/', $line, $matches)) {
    	break;
    }
    $path = '/'. $matches[3];
    $op_items[$path] = new VersioncontrolGitItem(
      VERSIONCONTROL_ITEM_FILE,
      $path,
      $revision,
      ($merge ? VERSIONCONTROL_ACTION_MERGED : VERSIONCONTROL_ACTION_MODIFIED),
      $repository
    );
    if (is_numeric($matches[1]) && is_numeric($matches[2])) {
      $op_items[$path]->line_changes = array(
          'added' => $matches[1],
           'removed' => $matches[2]
      );
    }
  } while (($line = next($logs)) !== FALSE);
  // Parse file actions.
  do {
    if (!preg_match('/^ (\S+) (\S+) (\S+) (.+)$/', $line, $matches)) {
    	break;
    }
    // We also can get 'mode' here if someone changes the file permissions.
    if ($matches[1] == 'create') {
      $op_items['/'. $matches[4]]->action = VERSIONCONTROL_ACTION_ADDED;
    }
    else if ($matches[1] == 'delete') {
      $op_items['/'. $matches[4]]->action = VERSIONCONTROL_ACTION_DELETED;
    }
  }
  while (($line = next($logs)) !== FALSE);

  // Fill in the source_items for non-added items
  foreach ($op_items as $path => $item) {
    if ($item->action != VERSIONCONTROL_ACTION_ADDED) {
      _versioncontrol_git_fill_source_item($item, $parents);
    }
  }
  return $op_items;
}

/**
 * Parse the output of 'git log' and insert a commit based on it's data.
 *
 * @param VersioncontrolRepository $repository
 * @param array $logs The output of 'git log' to parse
 * @param array $commits_in_db
 * @param array $branch_label_list An associative list of branchname => VersioncontrolBranch
 */
function _versioncontrol_git_log_parse_and_insert_commit($repository, $logs, &$commits_in_db, $branch_label_list) {
  $merge = FALSE;
  // Get Revision
  $revision = trim(next($logs));

  // Get $parents
  $parents = explode(" ", trim(next($logs)));
  if ($parents[0] == '') {
    $parents = array();
  }
  if (isset($parents[1])) {
    $merge = TRUE;
  }

  // Get Author
  $username = trim(next($logs));

  // Get Date as Timestamp
  $date = trim(next($logs));

  // Get revision message.
  // TODO: revisit!
  $message = '';
  $i = 0;
  while (($line = trim(next($logs))) !== FALSE) {
    if ($line == 'ENDOFOUTPUTGITMESSAGEHERE') {
      if (substr($message, -2) === "\n\n") {
        $message = substr($message, 0, strlen($message) - 1);
      }
      break;
    }
    if ($i == 1) {
      $message .= "\n";
    }
    $message .= $line ."\n";
    $i++;
  }

  // This is either a (kind of) diffstat for each modified file or a list of
  // file actions like moved, created, deleted, mode changed.
  $line = next($logs);
  // Get the items
  $op_items = _versioncontrol_git_parse_items($repository, $logs, $line, $revision,
    $parents, $merge);

  $op = new VersioncontrolGitOperation(VERSIONCONTROL_OPERATION_COMMIT, $username, $date, $revision, $message, $username, $repository);
  $op->labels = _versioncontrol_git_log_get_branches_of_commit($revision, $branch_label_list);
  $op->insert($op_items);

  $commits_in_db[] = $revision;
}



/// All branch related functions

/**
 * Attaches the label $branch to every commit if it is not there yet.
 * @param VersioncontrolBranch $branch
 * @param array $commits
 * @return none
 */
function _versioncontrol_git_log_attach_branch_to_commits($branch, $commits) {
  $constraints = array(
    'vcs' => array('git'),
    'repo_ids' => array($branch->repository->repo_id),
    'types' => array(VERSIONCONTROL_OPERATION_COMMIT),
    'revisions' => $commits,
  );
  $commits_as_op = VersioncontrolOperationCache::getInstance()->getOperations($constraints);
  foreach($commits_as_op as $vc_op_id => $op) {
  	// We need this complicated logic to avoid adding a branch twice to a commit.
  	$already_there = false;
    foreach($op->labels as $label) {
      if($label->type == VERSIONCONTROL_LABEL_BRANCH && $label->name == $branch->name) {
      	$already_there = true;
      }
    }
    if(!$already_there) {
    	$op->labels[] = $branch;
    	$op->updateLabels($op->labels);
    }
  }
}

/**
 * This function removes the given branch from all commits.
 * @param VersioncontrolRepository $repository
 * @param string $branch
 * @return none
 */
function _versioncontrol_git_log_remove_branch_from_commits($repository, $branch) {
  $constraints = array(
    'vcs' => array('git'),
    'repo_ids' => array($repository->repo_id),
    'types' => array(VERSIONCONTROL_OPERATION_COMMIT),
    'branches' => array($branch),
  );
  $commits_as_op = VersioncontrolOperationCache::getInstance()->getOperations($constraints);
  foreach($commits_as_op as $commit_op) {
    $new_labels = array();
    foreach($commit_op->labels as $label) {
      if($label->type == VERSIONCONTROL_LABEL_BRANCH && $label->name != $branch) {
        $new_labels[] = $label;
      }
    }
    $commit_op->updateLabels($new_labels);
  }
}


/// Tag related functions

/**
 * Returns a list of all existing tags in the database.
 * @param VersioncontrolRepository $repository
 * @return array An array of tag names.
 */
function _versioncontrol_git_log_get_tags_in_db($repository) {
  return _versioncontrol_git_log_get_labels_in_db($repository, VERSIONCONTROL_LABEL_TAG);
}

/**
 * Get all tags present in the repository.
 * @return array
 */
function _versioncontrol_git_log_get_tags_in_repo() {
  $log = _versioncontrol_git_log_exec('git show-ref --tags'); // Query tags.
  $tags = array();
  while (($line = next($log)) !== FALSE) {
  	// the output of git show-ref --tags looks like this
  	// 94a5915923d5a9a6af935e4055c95582fd1a1136 refs/tags/DRUPAL-5--1-0
  	// to get 'DRUPAL-5--1-0', we have to skip 51 chars.
    $tags[] = substr(trim($line), 51);
  }
  return $tags;
}


/**
 * Returns a string with fully qualified tag names from an array of tag names.
 * @param array $tags
 * @return string
 */
function _versioncontrol_git_get_tag_string($tags) {
  $tag_string = '';
  // $tag_string is a list of fully qualified tag names
  foreach ($tags as $tag) {
    $tag_string .= escapeshellarg("refs/tags/$tag") . ' ';
  }
  return $tag_string;
}

/**
 * Returns a list of tag names with the tagged commits.
 * Handles annotated tags.
 * @param array $tags An array of tag names
 * @return array A list of all tags with the respective tagged commit.
 */
function _versioncontrol_git_log_get_tag_commit_list($tags) {
  if(empty($tags)) {
    return array();
  }
  $tag_string = _versioncontrol_git_get_tag_string($tags);
  $exec = "git show-ref -d $tag_string";
  $tag_commit_list_raw = _versioncontrol_git_log_exec($exec);
  $tag_commit_list = array();
  $tags_annotated = array();
  foreach($tag_commit_list_raw as $tag_commit_line) {
  	if($tag_commit_line == '') {
  	  continue;
  	}
  	$tag_commit = substr($tag_commit_line, 0, 40);
  	// annotated tag mark
  	// 9c70f55549d3f4e70aaaf30c0697f704d02e9249 refs/tags/tag^{}
  	if (substr($tag_commit_line, -3, 3) == '^{}') {
  	  $tag_name = substr($tag_commit_line, 51, -3);
      $tags_annotated[$tag_name] = $tag_commit;
  	}
  	// Simple tags
  	// 9c70f55549d3f4e70aaaf30c0697f704d02e9249 refs/tags/tag
  	else {
      $tag_name = substr($tag_commit_line, 51);
  	}
    $tag_commit_list[$tag_name] = $tag_commit;
  }
  // Because annotated tags show up twice in the output of git show-ref, once
  // with a 'tag' object and once with a commit-id we will go through them and
  // adjust the array so we just keep the commits.
  foreach($tags_annotated as $tag_name => $tag_commit) {
  	$tag_commit_list[$tag_name] = $tag_commit;
  }
  return $tag_commit_list;
}

/**
 * Does all processing to insert the tags in $tags_new in the database.
 * @param VersioncontrolRepository $repository
 * @param array $tags_new All new tags.
 * @return none
 */
function _versioncontrol_git_log_process_tags($repository, $tags_new) {
  if (empty($tags_new)) {
    return array();
  }
  $tag_ops = array();

  // get a list of all tag names with the corresponding commit.
  $tag_commit_list = _versioncontrol_git_log_get_tag_commit_list($tags_new);
  $format = '%(objecttype)%0a%(objectname)%0a%(refname)%0a%(taggername) %(taggeremail)%0a%(taggerdate)%0a%(contents)ENDOFGITTAGOUTPUTMESAGEHERE';
  foreach($tag_commit_list as $tag_name => $tag_commit) {
    $exec = "git for-each-ref --format=\"$format\" refs/tags/" . escapeshellarg($tag_name);
  	$logs_tag_msg = _versioncontrol_git_log_exec($exec);
    $tagged_commit_op = _versioncontrol_git_log_get_commit($repository, $tag_commit);
  	// Get the specific tag data for annotated vs not annotated tags.
    if($logs_tag_msg[1] == 'commit') {
      // simple tag
      // [2] is tagged commit [3] tagname [4] and [5] empty [6] commit log message
      // We get the tagger, the tag_date and the tag_message from the tagged commit.
      $tagger = $tagged_commit_op->author;
      $tag_date = $tagged_commit_op->date + 1;
      $message = $tagged_commit_op->message;
    }
    else if($logs_tag_msg[1] == 'tag') {
      // annotated tag
      // [2] is the tagged commit [3] tag name
      $tagger = $logs_tag_msg[4];
      $tag_date = strtotime($logs_tag_msg[5]);
      // Get the tag message
      $message = '';
      $i = 0;
      while (true) {
      	$line = $logs_tag_msg[$i + 6];
      	if($logs_tag_msg[$i + 7] == 'ENDOFGITTAGOUTPUTMESAGEHERE') {
      		$message .= $line;
      		break;
      	}
      	$message .= $line ."\n";
        $i++;
      }
    }
    else {
      drupal_set_message(t('Serious problem in tag parsing, please check that you\'re using a supported version of git!', 'error'));
    }
    $tag_op = new VersioncontrolGitOperation (
      VERSIONCONTROL_OPERATION_TAG,
      $tagger,
      $tag_date,
      $tag_commit,
      $message,
      $tagger,
      $repository
    );
    $tag_op->labels = array(new VersioncontrolTag($tag_name, VERSIONCONTROL_ACTION_ADDED, null, $repository));
    $empty_array = array();
    $tag_op->insert($empty_array);
    // Update the tagged commit to include the label
    $tagged_commit_op->labels[] = new VersioncontrolTag($tag_name, VERSIONCONTROL_ACTION_MODIFIED, null, $repository);
    $tagged_commit_op->updateLabels($tagged_commit_op->labels);
  }
}
