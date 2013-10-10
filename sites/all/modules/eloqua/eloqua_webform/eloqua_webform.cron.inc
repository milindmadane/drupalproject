<?php
/**
 * @file
 *
 * Cron Support Functions for Eloqua
 *
 * @package Eloqua
 */

/**
 * Cron helper function
 *
 * @param object
 *   $saved post
 */
function _eloqua_cron($post=NULL) {
  // Ensure that curl exists.  Otherwise this will fail hard.
  if (!function_exists('curl_init')) {
    $message = t('Curl does not appear to be installed.');
    $params = array();
    watchdog('eloqua', $message, $params, WATCHDOG_ALERT);
    return;
  }

  $posts = array();

  // If post passed into this function, only process this post
  if ($post){
    $posts = array($post);
  }
  // Post all the data to Eloqua
  // Fetch Unposted Data
  else {
    $result_set = db_select('eloqua_saved_posts')
      ->fields('eloqua_saved_posts')
      ->condition('status', ELOQUA_STATUS_NEW)
      ->range(0, (int) variable_get('batch_size', 50))
      ->execute()
      ->fetchAll();
    $posts = _eloqua_unserialize_data_column($result_set);
  }

  // If nothnig to do, or something funky happened, bail.
  if (empty($posts) || !is_array($posts)) {
    return;
  }

  foreach ($posts as $post) {
    if (!is_array($post->data)) {
      $post->data = array();
    }

    // Only pass along the accept-language header as a header. We pass the user
    // agent information in the curl request below.
    $original_headers = $post->data['user_headers'];
    $headers = array(
      'Accept-Language' => array_key_exists('accept-language', $original_headers) ? $original_headers['accept-language'] : 'en',
    );

    // Fetch the post fields to send to Eloqua
    $post_fields = _eloqua_cron_get_post_fields($post);

    // Create the Curl Request
    $options = array();
    $options['post_fields'] = $post_fields;
    $options['user_agent'] = array_key_exists('user-agent', $original_headers) ? $original_headers['user-agent'] : 'User Relay';
    $options['http_headers'] = $headers;
    $ch = _eloqua_cron_get_curl_resource($options);
    if (is_null($ch)) {
      $message = t('Something went wrong with curl, unable to obtain handle.  Aborting status updates.');
      $params = array();
      watchdog('eloqua', $message, $params, WATCHDOG_ERROR);
      break;
    }
    try {
      $data = curl_exec($ch);
      $curl_status = curl_getinfo($ch);
      $status_class = (int) floor($curl_status['http_code'] / 100);
      $success = ($data) && ($status_class == 2 || $status_class == 3);
    } catch (Exception $e) {
      // PHP needs to implement finally
      // No idea what went wrong, should just bail out of the process.
      // However, log.
      $message = t('Something went wrong with curl. Uncaught exception: !message');
      $params = array(
        '!message' => $e->getMessage(),
      );
      watchdog('eloqua', $message, $params, WATCHDOG_ERROR);

      // Throwing an error here will result in an uncaught exception.  This, when it happens in
      // cron, will result in the "Cron exceeding time limit" error, because it didn't end properly.
      // The above will log the message to watchdog, and that's the extent of the notice that
      // will be given.
      // To mimic current behaviour, break will be used instead of continue.
      // throw $e;
      break;
    }
    curl_close($ch);

    // CURL
    $post->data['server_post'][] = array(
      'timestamp' => time(),
      'response' => $data,
      'http_status' => $curl_status['http_code'],
    );

    // Update Post Data
    if ($success) {
      $post->{'status'} = ELOQUA_STATUS_UPLOADED;
    } else {
      $post->{'status'} = ELOQUA_STATUS_FAILED;
    }
    eloqua_post_update($post);
  }
}

/**
 * Returns a configured Curl Resource for use
 *
 * @return resource
 */
function _eloqua_cron_get_curl_resource($options = array()) {
  $ch = curl_init();
  if (empty($ch)) {
    $message = t('Unable to open curl.  Skipping message.');
    $params = array();
    watchdog('eloqua', $message, $params, WATCHDOG_ERROR);
    $ch = NULL;
  }

  if ($ch == NULL) {
    return NULL;
  }

  // Setting Options
  $curl_opts = array(
    CURLOPT_HEADER => 1,
    CURLOPT_POST => 1,
    CURLOPT_RETURNTRANSFER => 1,
    CURLOPT_CONNECTTIMEOUT => 5, // TODO: TIMEOUT ADMIN SETTING
    CURLOPT_TIMEOUT => 5, // TODO: TIMEOUT ADMIN SETTING
    CURLOPT_URL => 'http://now.eloqua.com/e/f2.aspx',
  );

  $option_map = array(
    'post_fields' => CURLOPT_POSTFIELDS,
    'user_agent' => CURLOPT_USERAGENT,
    'http_headers' => CURLOPT_HTTPHEADER,
  );
  foreach ($option_map as $key => $curl_key) {
    if (array_key_exists($key, $options)) {
      $curl_opts[$curl_key] = $options[$key];
    }
  }

  $status = _eloqua_set_curl_opts($ch, $curl_opts);
  $error_msg = _eloqua_set_curl_get_error($status);
  if (!empty($error_msg)) {
    // Failed setting an option
    $message = t('Unable to set curl options. Skipping message. !options.');
    $params = array(
      '!options' => '<pre>' . $error_msg . '</pre>',
    );
    watchdog('eloqua', $message, $params, WATCHDOG_ERROR);
    curl_close($ch);
    $ch = NULL;
  }
  return $ch;
}

/**
 * Handle translating our post values into what eloqua wants in terms of structure.
 *
 * Dates are by default handled in YYYY-mm-dd
 * Times are by default handled in HH:mm
 *
 * @param $tree - the post tree name => value pairs
 * @param $posted_values - the post tree, could be name => value pairs or index => value pairs
 * @param $result - the re-structured tree that Eloqua will leverage
 * @return none
 */
function _eloqua_get_submission_data($tree, $posted_values, &$result) {
  foreach ($tree as $name => $value) {
    // we need to expand things in fieldsets
    if (is_array($value) && !in_array($value, $posted_values)) {
      _eloqua_get_submission_data($value, $posted_values, $result);
    }
    // we need to convert multi-value fields into strings
    else if (is_array($value)) {
      $result[$name] = implode(',', $value);
    }
    // everything else is just passed along
    else {
      $result[$name] = $value;
    }
  }
}

/**
 * Remaps the post fields to be used within Eloqua
 *
 * @param $post_fields array
 *   Data to post
 * @param $post array
 *   Post data
 * @return array
 *   $post_fields modified to have remapped keys where defined
 */
function _eloqua_cron_remap_post_fields($post_fields, $post) {
  $result = array();
  $nid = $post->data['form_post']['details']['nid'];
  if (empty($nid) || !is_numeric($nid)) {
    // Something is wrong with this
    return $post_fields;
  }
  $node = node_load($nid);
  $components = $node->webform['components'];
  $map = array();
  if (is_array($components)) {
    foreach ($components as $component) {
      if (!empty($component['extra']['eloqua']['key'])
        && $component['extra']['eloqua']['key'] !== $component['form_key']) {
        $map[$component['form_key']] = $component['extra']['eloqua']['key'];
      }
    }
  }

  foreach ($post_fields as $key => $value) {
    if (isset($map[$key])) {
      $result[$map[$key]] = $value;
    } else {
      $result[$key] = $value;
    }
  }

  return $result;
}

/**
 * Returns the cURL error for a status returned from eloqua_curl_set_opts
 *
 * @TODO: Theme
 * @param $status array
 * @return string
 *   Html formatted message
 */
function _eloqua_set_curl_get_error($status) {
  if (!is_array($status)) {
    return FALSE;
  }
  $messages = array();
  $message = '';
  foreach ($status as $key => $data) {
    if ($data['status']) {
      continue;
    }
    $value = var_export($data['value'], TRUE);
    $messages[] = "<li>$key - Error: {$data['message']}<br /> Value: $value</li>";
  }
  if (!empty($messages)) {
    $message = '<ul>' . implode("\n", $messages) . '</ul>';
  }
  return $message;
}

/**
 * Sets curl options en mass, checking for errors
 *
 * @param $ch resource
 *  Curl Resource
 * @param $options array
 *  Curl settings to apply to the resource
 * @return array
 *  Status of all the set_opt calls
 *
 */
function _eloqua_set_curl_opts(&$ch, $options = array()) {
  if (!is_array($options)) {
    return FALSE;
  }
  $result = array();
  foreach ($options as $key => $value) {
    $status = curl_setopt($ch, $key, $value);
    $status_msg = '';
    if (!$status) {
      $status_msg = curl_error($ch);
    }
    $result[$key] = array(
      'status' => $status,
      'value' => $value,
      'message' => $status_msg,
    );
  }
  return $result;
}

/**
 * Returns the post fields for the request, modified for eloqua
 *
 * @param  $post
 * @return array
 */
function _eloqua_cron_get_post_fields($post) {
  // TODO: Review newest webform for exact behaviour on whether submitted_tree exists.
  $form_data = !empty($post->data['form_post']['submitted_tree']) ? $post->data['form_post']['submitted_tree'] : $post->data['form_post']['submitted'];

  $post_fields = array();
  // Merge any elq values into the post to be submitted
  foreach ($post->data['form_post'] as $name => $value) {
    if (substr($name, 0, 3) === 'elq') {
      $post_fields[$name] = $value;
    }
  }

  // translates the data into a Eloqua-worthy format
  _eloqua_get_submission_data($form_data, $post->data['form_post']['submitted'], $post_fields);

  // Remap any fields that can't be handled by webform
  $post_fields = _eloqua_cron_remap_post_fields($post_fields, $post);
  return $post_fields;
}
