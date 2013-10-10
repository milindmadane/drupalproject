<?php
/**
 * @file
 *
 * General administration form for Eloqua settings.
 *
 * @return array
 *   A form array.
 */
function eloqua_admin_form() {
  $form = array();
  $form['general'] = array(
    '#type' => 'fieldset',
    '#title' => t('General Settings'),
    '#description' => t('General settings applicable to all Eloqua functionality.'),
    'eloqua_site_id' => array(
      '#type' => 'textfield',
      '#title' => t('Site Identifier'),
      '#description' => t('The Eloqua Site ID for this web site.'),
      '#size' => 20,
      '#maxlength' => 64,
      '#default_value' => variable_get('eloqua_site_id', 0),
      '#required' => TRUE,
    ),
    'batch_size' => array(
      '#type' => 'textfield',
      '#title' => t('Batch Size'),
      '#description' => t('The number of posts to process per cron run. Default=50'),
      '#size' => 5,
      '#maxlength' => 5,
      '#default_value' => (int) variable_get('batch_size', 50),
    )
  );
  return system_settings_form($form);
}
