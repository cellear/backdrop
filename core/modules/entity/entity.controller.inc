<?php

/**
 * @file
 * Entity API controller classes and interface.
 */

/**
 * Defines a common interface for entity controller classes.
 *
 * All entity controller classes specified via the 'controller class' key
 * returned by hook_entity_info() or hook_entity_info_alter() have to implement
 * this interface.
 *
 * Most simple, SQL-based entity controllers will do better by extending
 * DefaultEntityController instead of implementing this interface
 * directly.
 */
interface EntityControllerInterface {

  /**
   * Resets the internal, static entity cache.
   *
   * @param $ids
   *   (optional) If specified, the cache is reset for the entities with the
   *   given ids only.
   */
  public function resetCache(array $ids = NULL);

  /**
   * Loads one or more entities.
   *
   * @param $ids
   *   An array of entity IDs, or FALSE to load all entities.
   * @param $conditions
   *   An array of conditions in the form 'field' => $value.
   *
   * @return
   *   An array of entity objects indexed by their ids.
   */
  public function load($ids = array(), $conditions = array());
}

/**
 * Defines a base entity controller class.
 *
 * Default implementation of EntityControllerInterface.
 *
 * This class can be used as-is by most simple entity types. Entity types
 * requiring special handling can extend the class.
 */
class DefaultEntityController implements EntityControllerInterface {

  /**
   * Static cache of entities.
   *
   * @var array
   */
  protected $entityCache;

  /**
   * Entity type for this controller instance.
   *
   * @var string
   */
  protected $entityType;

  /**
   * Array of information about the entity.
   *
   * @var array
   *
   * @see entity_get_info()
   */
  protected $entityInfo;

  /**
   * Additional arguments to pass to hook_TYPE_load().
   *
   * Set before calling DefaultEntityController::attachLoad().
   *
   * @var array
   */
  protected $hookLoadArguments;

  /**
   * Name of the entity's ID field in the entity database table.
   *
   * @var string
   */
  protected $idKey;

  /**
   * Name of entity's revision database table field, if it supports revisions.
   *
   * Has the value FALSE if this entity does not use revisions.
   *
   * @var string
   */
  protected $revisionKey;

  /**
   * The table that stores revisions, if the entity supports revisions.
   *
   * @var string
   */
  protected $revisionTable;

  /**
   * Whether this entity type should use the static cache.
   *
   * Set by entity info.
   *
   * @var boolean
   */
  protected $cache;

  /**
   * Implements EntityControllerInterface::__construct().
   *
   * Sets basic variables.
   *
   * @param $entityType
   *   The entity type for which the instance is created.
   */
  public function __construct($entityType) {
    $this->entityType = $entityType;
    $this->entityInfo = entity_get_info($entityType);
    $this->entityCache = array();
    $this->hookLoadArguments = array();
    $this->idKey = $this->entityInfo['entity keys']['id'];

    // Check if the entity type supports revisions.
    if (!empty($this->entityInfo['entity keys']['revision'])) {
      $this->revisionKey = $this->entityInfo['entity keys']['revision'];
      $this->revisionTable = $this->entityInfo['revision table'];
    }
    else {
      $this->revisionKey = FALSE;
    }

    // Check if the entity type supports static caching of loaded entities.
    $this->cache = !empty($this->entityInfo['static cache']);
  }

  /**
   * Implements EntityControllerInterface::resetCache().
   */
  public function resetCache(array $ids = NULL) {
    if (isset($ids)) {
      foreach ($ids as $id) {
        unset($this->entityCache[$id]);
      }
    }
    else {
      $this->entityCache = array();
    }
  }

  /**
   * Implements EntityControllerInterface::load().
   */
  public function load($ids = array(), $conditions = array()) {
    $entities = array();

    // Revisions are not statically cached, and require a different query to
    // other conditions, so separate the revision id into its own variable.
    if ($this->revisionKey && isset($conditions[$this->revisionKey])) {
      $revision_id = $conditions[$this->revisionKey];
      unset($conditions[$this->revisionKey]);
    }
    else {
      $revision_id = FALSE;
    }

    // Create a new variable which is either a prepared version of the $ids
    // array for later comparison with the entity cache, or FALSE if no $ids
    // were passed. The $ids array is reduced as items are loaded from cache,
    // and we need to know if it's empty for this reason to avoid querying the
    // database when all requested entities are loaded from cache.
    $passed_ids = !empty($ids) ? array_flip($ids) : FALSE;
    // Try to load entities from the static cache, if the entity type supports
    // static caching.
    if ($this->cache && !$revision_id) {
      $entities += $this->cacheGet($ids, $conditions);
      // If any entities were loaded, remove them from the ids still to load.
      if ($passed_ids) {
        $ids = array_keys(array_diff_key($passed_ids, $entities));
      }
    }

    // Load any remaining entities from the database. This is the case if $ids
    // is set to FALSE (so we load all entities), if there are any ids left to
    // load, if loading a revision, or if $conditions was passed without $ids.
    if ($ids === FALSE || $ids || $revision_id || ($conditions && !$passed_ids)) {
      // Build and execute the query.
      $query_result = $this->buildQuery($ids, $conditions, $revision_id)->execute();

      if (!empty($this->entityInfo['entity class'])) {
        // We provide the necessary arguments for PDO to create objects of the
        // specified entity class.
        // @see EntityInterface::__construct()
        $query_result->setFetchMode(PDO::FETCH_CLASS, $this->entityInfo['entity class'], array(array(), $this->entityType));
      }
      $queried_entities = $query_result->fetchAllAssoc($this->idKey);
    }

    // Pass all entities loaded from the database through $this->attachLoad(),
    // which attaches fields (if supported by the entity type) and calls the
    // entity type specific load callback, for example hook_node_load().
    if (!empty($queried_entities)) {
      $this->attachLoad($queried_entities, $revision_id);
      $entities += $queried_entities;
    }

    if ($this->cache) {
      // Add entities to the cache if we are not loading a revision.
      if (!empty($queried_entities) && !$revision_id) {
        $this->cacheSet($queried_entities);
      }
    }

    // Ensure that the returned array is ordered the same as the original
    // $ids array if this was passed in and remove any invalid ids.
    if ($passed_ids) {
      // Remove any invalid ids from the array.
      $passed_ids = array_intersect_key($passed_ids, $entities);
      foreach ($entities as $entity) {
        $passed_ids[$entity->{$this->idKey}] = $entity;
      }
      $entities = $passed_ids;
    }

    return $entities;
  }

  /**
   * Builds the query to load the entity.
   *
   * This has full revision support. For entities requiring special queries,
   * the class can be extended, and the default query can be constructed by
   * calling parent::buildQuery(). This is usually necessary when the object
   * being loaded needs to be augmented with additional data from another
   * table, such as loading node type into comments or vocabulary machine name
   * into terms, however it can also support $conditions on different tables.
   * See CommentController::buildQuery() or TaxonomyTermController::buildQuery()
   * for examples.
   *
   * @param $ids
   *   An array of entity IDs, or FALSE to load all entities.
   * @param $conditions
   *   An array of conditions in the form 'field' => $value.
   * @param $revision_id
   *   The ID of the revision to load, or FALSE if this query is asking for the
   *   most current revision(s).
   *
   * @return SelectQuery
   *   A SelectQuery object for loading the entity.
   */
  protected function buildQuery($ids, $conditions = array(), $revision_id = FALSE) {
    $query = db_select($this->entityInfo['base table'], 'base');

    $query->addTag($this->entityType . '_load_multiple');

    if ($revision_id) {
      $query->join($this->revisionTable, 'revision', "revision.{$this->idKey} = base.{$this->idKey} AND revision.{$this->revisionKey} = :revisionId", array(':revisionId' => $revision_id));
    }
    elseif ($this->revisionKey) {
      $query->join($this->revisionTable, 'revision', "revision.{$this->revisionKey} = base.{$this->revisionKey}");
    }

    // Add fields from the {entity} table.
    $entity_fields = $this->entityInfo['schema_fields_sql']['base table'];

    if ($this->revisionKey) {
      // Add all fields from the {entity_revision} table.
      $entity_revision_fields = backdrop_map_assoc($this->entityInfo['schema_fields_sql']['revision table']);
      // The id field is provided by entity, so remove it.
      unset($entity_revision_fields[$this->idKey]);

      // Remove all fields from the base table that are also fields by the same
      // name in the revision table.
      $entity_field_keys = array_flip($entity_fields);
      foreach ($entity_revision_fields as $key => $name) {
        if (isset($entity_field_keys[$name])) {
          unset($entity_fields[$entity_field_keys[$name]]);
        }
      }
      $query->fields('revision', $entity_revision_fields);
    }

    $query->fields('base', $entity_fields);

    if ($ids) {
      $query->condition("base.{$this->idKey}", $ids, 'IN');
    }
    if ($conditions) {
      foreach ($conditions as $field => $value) {
        $query->condition('base.' . $field, $value);
      }
    }
    return $query;
  }

  /**
   * Attaches data to entities upon loading.
   *
   * This will attach fields, if the entity is fieldable. It calls
   * hook_entity_load() for modules which need to add data to all entities.
   * It also calls hook_TYPE_load() on the loaded entities. For example
   * hook_node_load() or hook_user_load(). If your hook_TYPE_load()
   * expects special parameters apart from the queried entities, you can set
   * $this->hookLoadArguments prior to calling the method.
   * See NodeController::attachLoad() for an example.
   *
   * @param $queried_entities
   *   Associative array of query results, keyed on the entity ID.
   * @param $revision_id
   *   ID of the revision that was loaded, or FALSE if the most current revision
   *   was loaded.
   */
  protected function attachLoad(&$queried_entities, $revision_id = FALSE) {
    // Attach fields.
    if ($this->entityInfo['fieldable']) {
      if ($revision_id) {
        field_attach_load_revision($this->entityType, $queried_entities);
      }
      else {
        field_attach_load($this->entityType, $queried_entities);
      }
    }

    // Call hook_entity_load().
    foreach (module_implements('entity_load') as $module) {
      $function = $module . '_entity_load';
      $function($queried_entities, $this->entityType);
    }
    // Call hook_TYPE_load(). The first argument for hook_TYPE_load() are
    // always the queried entities, followed by additional arguments set in
    // $this->hookLoadArguments.
    $args = array_merge(array($queried_entities), $this->hookLoadArguments);
    foreach (module_implements($this->entityInfo['load hook']) as $module) {
      call_user_func_array($module . '_' . $this->entityInfo['load hook'], $args);
    }
  }

  /**
   * Gets entities from the static cache.
   *
   * @param $ids
   *   If not empty, return entities that match these IDs.
   * @param $conditions
   *   If set, return entities that match all of these conditions.
   *
   * @return
   *   Array of entities from the entity cache.
   */
  protected function cacheGet($ids, $conditions = array()) {
    $entities = array();
    // Load any available entities from the internal cache.
    if (!empty($this->entityCache)) {
      if ($ids) {
        $entities += array_intersect_key($this->entityCache, array_flip($ids));
      }
      // If loading entities only by conditions, fetch all available entities
      // from the cache. Entities which don't match are removed later.
      elseif ($conditions) {
        $entities = $this->entityCache;
      }
    }

    // Exclude any entities loaded from cache if they don't match $conditions.
    // This ensures the same behavior whether loading from memory or database.
    if ($conditions) {
      foreach ($entities as $entity) {
        // Iterate over all conditions and compare them to the entity
        // properties. We cannot use array_diff_assoc() here since the
        // conditions can be nested arrays, too.
        foreach ($conditions as $property_name => $condition) {
          if (is_array($condition)) {
            // Multiple condition values for one property are treated as OR
            // operation: only if the value is not at all in the condition array
            // we remove the entity.
            if (!in_array($entity->{$property_name}, $condition)) {
              unset($entities[$entity->{$this->idKey}]);
              continue 2;
            }
          }
          elseif ($condition != $entity->{$property_name}) {
            unset($entities[$entity->{$this->idKey}]);
            continue 2;
          }
        }
      }
    }
    return $entities;
  }

  /**
   * Stores entities in the static entity cache.
   *
   * @param $entities
   *   Entities to store in the cache.
   */
  protected function cacheSet($entities) {
    $this->entityCache += $entities;
  }
}

/**
 * Defines a common interface for entity storage controllers.
 */
interface EntityStorageControllerInterface extends EntityControllerInterface {

  /**
   * Constructs a new entity object, without permanently saving it.
   *
   * @param $values
   *   An array of values to set, keyed by property name. If the entity type has
   *   bundles the bundle key has to be specified.
   *
   * @return EntityInterface
   *   A new entity object.
   */
  public function create(array $values);

  /**
   * Deletes permanently saved entities.
   *
   * @param $ids
   *   An array of entity IDs.
   *
   * @throws EntityStorageException
   *   In case of failures, an exception is thrown.
   */
  public function delete($ids);

  /**
   * Saves the entity permanently.
   *
   * @param EntityInterface $entity
   *   The entity to save.
   *
   * @return
   *   SAVED_NEW or SAVED_UPDATED is returned depending on the operation
   *   performed.
   *
   * @throws EntityStorageException
   *   In case of failures, an exception is thrown.
   */
  public function save(EntityInterface $entity);

}

/**
 * Defines an exception thrown when storage operations fail.
 */
class EntityStorageException extends Exception { }

/**
 * Implements the entity storage controller interface for the database.
 */
class EntityDatabaseStorageController extends DefaultEntityController implements EntityStorageControllerInterface {

  /**
   * Implements EntityStorageControllerInterface::create().
   */
  public function create(array $values) {
    $class = isset($this->entityInfo['entity class']) ? $this->entityInfo['entity class'] : 'Entity';
    return new $class($values);
  }

  /**
   * Implements EntityStorageControllerInterface::delete().
   */
  public function delete($ids) {
    $entities = $ids ? $this->load($ids) : FALSE;
    if (!$entities) {
      // If no IDs or invalid IDs were passed, do nothing.
      return;
    }
    $transaction = db_transaction();

    try {
      $this->preDelete($entities);
      foreach ($entities as $id => $entity) {
        $this->invokeHook('predelete', $entity);
      }
      $ids = array_keys($entities);

      db_delete($this->entityInfo['base table'])
        ->condition($this->idKey, $ids, 'IN')
        ->execute();
      // Reset the cache as soon as the changes have been applied.
      $this->resetCache($ids);

      $this->postDelete($entities);
      foreach ($entities as $id => $entity) {
        $this->invokeHook('delete', $entity);
      }
      // Ignore slave server temporarily.
      db_ignore_slave();
    }
    catch (Exception $e) {
      $transaction->rollback();
      watchdog_exception($this->entityType, $e);
      throw new EntityStorageException($e->getMessage, $e->getCode, $e);
    }
  }

  /**
   * Implements EntityStorageControllerInterface::save().
   */
  public function save(EntityInterface $entity) {
    $transaction = db_transaction();
    try {
      // Load the stored entity, if any.
      if (!$entity->isNew() && !isset($entity->original)) {
        $entity->original = entity_load_unchanged($this->entityType, $entity->id());
      }

      $this->preSave($entity);
      $this->invokeHook('presave', $entity);

      if (!$entity->isNew()) {
        $return = backdrop_write_record($this->entityInfo['base table'], $entity, $this->idKey);
        $this->resetCache(array($entity->{$this->idKey}));
        $this->postSave($entity, TRUE);
        $this->invokeHook('update', $entity);
      }
      else {
        $return = backdrop_write_record($this->entityInfo['base table'], $entity);
        // Reset general caches, but keep caches specific to certain entities.
        $this->resetCache(array());
        $this->postSave($entity, FALSE);
        $this->invokeHook('insert', $entity);
      }

      // Ignore slave server temporarily.
      db_ignore_slave();
      unset($entity->is_new);
      unset($entity->original);

      return $return;
    }
    catch (Exception $e) {
      $transaction->rollback();
      watchdog_exception($this->entityType, $e);
      throw new EntityStorageException($e->getMessage(), $e->getCode(), $e);
    }
  }

  /**
   * Acts on an entity before the presave hook is invoked.
   *
   * Used before the entity is saved and before invoking the presave hook.
   */
  protected function preSave(EntityInterface $entity) { }

  /**
   * Acts on a saved entity before the insert or update hook is invoked.
   *
   * Used after the entity is saved, but before invoking the insert or update
   * hook.
   *
   * @param $update
   *   (bool) TRUE if the entity has been updated, or FALSE if it has been
   *   inserted.
   */
  protected function postSave(EntityInterface $entity, $update) { }

  /**
   * Acts on entities before they are deleted.
   *
   * Used before the entities are deleted and before invoking the delete hook.
   */
  protected function preDelete($entities) { }

  /**
   * Acts on deleted entities before the delete hook is invoked.
   *
   * Used after the entities are deleted but before invoking the delete hook.
   */
  protected function postDelete($entities) { }

  /**
   * Invokes a hook on behalf of the entity.
   *
   * @param $hook
   *   One of 'presave', 'insert', 'update', 'predelete', or 'delete'.
   * @param $entity
   *   The entity object.
   */
  protected function invokeHook($hook, EntityInterface $entity) {
    if (!empty($this->entityInfo['fieldable']) && function_exists($function = 'field_attach_' . $hook)) {
      $function($this->entityType, $entity);
    }
    // Invoke the hook.
    module_invoke_all($this->entityType . '_' . $hook, $entity);
    // Invoke the respective entity-level hook.
    module_invoke_all('entity_' . $hook, $entity, $this->entityType);
  }
}
