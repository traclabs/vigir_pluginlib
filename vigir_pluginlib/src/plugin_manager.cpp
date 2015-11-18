#include <vigir_pluginlib/plugin_manager.h>
#include <ros/console.h>

namespace vigir_pluginlib
{
using namespace vigir_generic_params;

PluginManager::Ptr PluginManager::singelton = PluginManager::Ptr();

PluginManager::PluginManager()
{
}

PluginManager::~PluginManager()
{
  // prevents warning when ClassLoaders get destroyed
  plugins_by_name.clear();

  for (PluginLoaderBase* loader : Instance()->class_loader)
    delete loader;
  class_loader.clear();
}

PluginManager::Ptr& PluginManager::Instance()
{
   if (!singelton)
      singelton.reset(new PluginManager());
   return singelton;
}

void PluginManager::initialize(ros::NodeHandle& nh)
{
  Instance()->nh = nh;

  // subscribe topics
  Instance()->add_plugin_sub = nh.subscribe("plugin_manager/add_plugin", 1, &PluginManager::addPlugin, Instance().get());
  Instance()->remove_plugin_sub = nh.subscribe("plugin_manager/remove_plugin", 1, &PluginManager::removePlugin, Instance().get());

  Instance()->plugin_states_pub = nh.advertise<msgs::PluginStates>("plugin_manager/plugin_states_update", 1);

  // start own services
  Instance()->get_plugin_descriptions_srv = nh.advertiseService("plugin_manager/get_plugin_descriptions", &PluginManager::getPluginDescriptionsService, Instance().get());
  Instance()->get_plugin_states_srv = nh.advertiseService("plugin_manager/get_plugin_states", &PluginManager::getPluginStatesService, Instance().get());
  Instance()->add_plugin_srv = nh.advertiseService("plugin_manager/add_plugin", &PluginManager::addPluginService, Instance().get());
  Instance()->remove_plugin_srv = nh.advertiseService("plugin_manager/remove_plugin", &PluginManager::removePluginService, Instance().get());
  Instance()->load_plugin_set_srv = nh.advertiseService("plugin_manager/load_plugin_set", &PluginManager::loadPluginSetService, Instance().get());

  // init action servers
  Instance()->get_plugin_descriptions_as.reset(new GetPluginDescriptionsActionServer(nh, "plugin_manager/get_plugin_descriptions", boost::bind(&PluginManager::getPluginDescriptionsAction, Instance().get(), _1), false));
  Instance()->get_plugin_states_as.reset(new GetPluginStatesActionServer(nh, "plugin_manager/get_plugin_states", boost::bind(&PluginManager::getPluginStatesAction, Instance().get(), _1), false));
  Instance()->add_plugin_as.reset(new PluginManagementActionServer(nh, "plugin_manager/add_plugin", boost::bind(&PluginManager::addPluginAction, Instance().get(), _1), false));
  Instance()->remove_plugin_as.reset(new PluginManagementActionServer(nh, "plugin_manager/remove_plugin", boost::bind(&PluginManager::removePluginAction, Instance().get(), _1), false));
  Instance()->load_plugin_set_as.reset(new PluginManagementActionServer(nh, "plugin_manager/load_plugin_set", boost::bind(&PluginManager::loadPluginSetAction, Instance().get(), _1), false));

  // start action servers
  Instance()->get_plugin_descriptions_as->start();
  Instance()->get_plugin_states_as->start();
  Instance()->add_plugin_as->start();
  Instance()->remove_plugin_as->start();
  Instance()->load_plugin_set_as->start();
}

const PluginManager::PluginLoaderVector& PluginManager::getPluginClassLoader()
{
  return Instance()->class_loader;
}

bool PluginManager::addPlugins(const std::vector<msgs::PluginDescription>& plugin_descriptions)
{
  bool success = true;
  for (const msgs::PluginDescription& description : plugin_descriptions)
  {
    if (!addPlugin(description))
      success = false;
  }
  return success;
}

bool PluginManager::addPlugin(const msgs::PluginDescription& plugin_description)
{
  const std::string& name = plugin_description.name.data;
  const std::string& type_class_package = plugin_description.type_class_package.data;
  const std::string& type_class = plugin_description.type_class.data;
  const std::string& base_class_package = plugin_description.base_class_package.data;
  const std::string& base_class = plugin_description.base_class.data;

  // try to add by name identifier located in config files
  if (!name.empty())
  {
    ros::NodeHandle plugin_nh(Instance()->nh, name);
    if (plugin_nh.hasParam("type_class"))
    {
      std::string _type_class;
      if (plugin_nh.getParam("type_class", _type_class) && (type_class.empty() || type_class == _type_class))
        return addPluginByName(name);
    }
  }

  // try to initialize by type
  if(!type_class.empty())
    return addPlugin(type_class_package, type_class, base_class_package, base_class, name);

  ROS_ERROR("[PluginManager] addPlugin: Call without name or type class!");
  return false;
}

bool PluginManager::addPlugin(const std::string& type_class_package, const std::string& type_class, const std::string& base_class_package, const std::string& base_class, const std::string& name)
{
  Plugin::Ptr p;

  try
  {
    std::string _base_class = base_class;

    // search for appropriate ClassLoader
    for (PluginLoaderBase* loader : Instance()->class_loader)
    {
      if (loader->isClassAvailable(type_class) && (base_class.empty() || base_class == loader->getBaseClassType()))
      {
        if (type_class_package != loader->getClassPackage(type_class))
        {
          ROS_WARN("type_class_package: %s != %s", type_class_package.c_str(), loader->getClassPackage(type_class).c_str());
          continue;
        }
        if (!base_class_package.empty() && base_class_package != loader->getBaseClassPackage())
        {
          ROS_WARN("base_class_package: %s != %s", base_class_package.c_str(), loader->getBaseClassPackage().c_str());
          continue;
        }
        if (!base_class.empty() && base_class != loader->getBaseClassType())
        {
          ROS_WARN("base_class: %s != %s", base_class.c_str(), loader->getBaseClassType().c_str());
          continue;
        }

        if (!p)
        {
          _base_class = loader->getBaseClassType().c_str();
          p = loader->createPluginInstance(type_class);

          if (!name.empty())
            p->name = name;

          p->description.name.data = p->name;
          p->description.base_class_package.data = loader->getBaseClassPackage();
          p->description.base_class.data = loader->getBaseClassType();
          p->description.type_class_package.data = loader->getClassPackage(type_class);
          p->description.type_class.data = type_class;
        }
        else
          ROS_WARN("[PluginManager] Duplicate source for plugin '%s' found in ClassLoader '%s'!\nPlugin was already instanciated from ClassLoader '%s'", type_class.c_str(), loader->getBaseClassType().c_str(), _base_class.c_str());
      }
    }
    if (!p)
    {
      ROS_ERROR("[PluginManager] Plugin of type_class '%s' is unknown! Check if ClassLoader has been initialized!", type_class.c_str());
      return false;
    }
  }
  catch (pluginlib::PluginlibException& e)
  {
    ROS_ERROR("[PluginManager] Plugin of type_class '%s' failed to load for some reason. Error: %s", type_class.c_str(), e.what());
    return false;
  }

  PluginManager::addPlugin(p);
  return true;
}

bool PluginManager::addPluginByName(const std::string& name)
{
  try
  {
    ros::NodeHandle plugin_nh(Instance()->nh, name);

    std::string type_class_package;
    std::string type_class;
    std::string base_class_package;
    std::string base_class;

    if (plugin_nh.getParam("type_class_package", type_class_package) && plugin_nh.getParam("type_class", type_class))
    {
      plugin_nh.param("base_class_package", base_class_package, std::string());
      plugin_nh.param("base_class", base_class, std::string());

      ROS_DEBUG("Constructing plugin '%s' of type_class_package: '%s', type_class: '%s' (base_class_package: '%s', base_class: '%s')", name.c_str(), type_class_package.c_str(), type_class.c_str(), base_class_package.c_str(), base_class.c_str());
      return addPlugin(type_class_package, type_class, base_class_package, base_class, name);
    }
    else
      ROS_ERROR("Could not add plugin '%s' due to missing type_class_package or type_class. Check if configuration is loaded to the parameter server (namespace: '%s')!", name.c_str(), plugin_nh.getNamespace().c_str());
  }
  catch (std::exception& e)
  {
    ROS_ERROR("Exception while constructing plugin with name '%s':\n%s", name.c_str(), e.what());
    return false;
  }

  ROS_ERROR("Failed to add plugin '%s'!", name.c_str());
  return false;
}

void PluginManager::addPlugin(Plugin* plugin)
{
  Plugin::Ptr plugin_ptr(plugin);
  addPlugin(plugin_ptr);
}

void PluginManager::addPlugin(Plugin::Ptr plugin)
{
  if (!plugin)
  {
    ROS_ERROR("[PluginManager] Got NULL pointer as plugin. Fix it immediatly!");
    return;
  }

  std::map<std::string, Plugin::Ptr>::iterator itr = Instance()->plugins_by_name.find(plugin->getName());
  Plugin::Ptr unique_plugin;

  if (itr != Instance()->plugins_by_name.end()) // replace by name
    ROS_INFO("[PluginManager] Plugin '%s' with type_id '%s' is replaced by '%s' with type_id '%s'!", itr->second->getName().c_str(), itr->second->getTypeId().c_str(), plugin->getName().c_str(), plugin->getTypeId().c_str());
  else if (plugin->isUnique() && getUniquePluginByTypeId(plugin->getTypeId(), unique_plugin)) // replace by uniqueness
  {
    ROS_INFO("[PluginManager] Unique plugin '%s' with type_id '%s' is replaced by '%s'!", unique_plugin->getName().c_str(), unique_plugin->getTypeId().c_str(), plugin->getName().c_str());
    Instance()->plugins_by_name.erase(Instance()->plugins_by_name.find(unique_plugin->getName())); // prevent outputs by removePlugin call
  }
  else
    ROS_INFO("[PluginManager] Added new plugin '%s' with type_id '%s'", plugin->getName().c_str(), plugin->getTypeId().c_str());

  Instance()->plugins_by_name[plugin->getName()] = plugin;

  plugin->initialize(Instance()->nh, ParameterManager::getActive());

  // publish update
  Instance()->publishPluginStateUpdate();
}

bool PluginManager::getPluginByName(const std::string& name, Plugin::Ptr& plugin)
{
  plugin.reset();

  std::map<std::string, Plugin::Ptr>::const_iterator itr = Instance()->plugins_by_name.find(name);
  if (itr == Instance()->plugins_by_name.end())
    return false;

  plugin = itr->second;
  return true;
}

bool PluginManager::getPluginsByTypeId(const std::string& type_id, std::vector<Plugin::Ptr>& plugins)
{
  plugins.clear();

  for (std::map<std::string, Plugin::Ptr>::iterator itr = Instance()->plugins_by_name.begin(); itr != Instance()->plugins_by_name.end(); itr++)
  {
    if (itr->second->getTypeId() == type_id)
      plugins.push_back(itr->second);
  }

  return !plugins.empty();
}

bool PluginManager::getUniquePluginByTypeId(const std::string& type_id, Plugin::Ptr& plugin)
{
  plugin.reset();

  for (std::map<std::string, Plugin::Ptr>::iterator itr = Instance()->plugins_by_name.begin(); itr != Instance()->plugins_by_name.end(); itr++)
  {
    if (itr->second->isUnique() && itr->second->getTypeId() == type_id)
    {
      plugin = itr->second;
      return true;
    }
  }

  return false;
}

void PluginManager::getPluginDescriptions(std::vector<msgs::PluginDescription>& descriptions, msgs::PluginDescription filter)
{
  descriptions.clear();

  for (PluginLoaderBase* loader : Instance()->class_loader)
  {
    msgs::PluginDescription description;
    description.base_class_package.data = loader->getBaseClassPackage();
    description.base_class.data = loader->getBaseClassType();

    if (!filter.base_class_package.data.empty() && filter.base_class_package.data != description.base_class_package.data)
      continue;
    if (!filter.base_class.data.empty() && filter.base_class.data != description.base_class.data)
      continue;

    for (std::string type_class : loader->getDeclaredClasses())
    {
      description.type_class_package.data = loader->getClassPackage(type_class);
      description.type_class.data = type_class;

      if (!filter.type_class_package.data.empty() && filter.type_class_package.data != description.type_class_package.data)
        continue;
      if (!filter.type_class.data.empty() && filter.type_class.data != description.type_class.data)
        continue;

      descriptions.push_back(description);
    }
  }
}

void PluginManager::getPluginStates(std::vector<msgs::PluginState>& plugin_states, msgs::PluginDescription filter)
{
  plugin_states.clear();

  for (std::map<std::string, Plugin::Ptr>::const_iterator itr = Instance()->plugins_by_name.begin(); itr != Instance()->plugins_by_name.end(); itr++)
  {
    const Plugin::Ptr& plugin = itr->second;

    msgs::PluginState state;

    state.header.stamp = ros::Time::now();
    state.description = plugin->getDescription();

    if (!vigir_pluginlib::isDescriptionMatching(state.description, filter))
      continue;

    plugin_states.push_back(state);
  }
}

bool PluginManager::removePlugins(const std::vector<msgs::PluginDescription>& plugin_descriptions)
{
  bool success = true;
  for (const msgs::PluginDescription& description : plugin_descriptions)
  {
    if (!removePlugin(description))
      success = false;
  }
  return success;
}

bool PluginManager::removePlugin(const msgs::PluginDescription& plugin_description)
{
  if (!plugin_description.name.data.empty())
    return removePluginByName(plugin_description.name.data);
//  else if(!plugin_description.type_class.data.empty())
//    addPlugin(plugin_description.type_class.data, plugin_description.base_class.data);
  else
    ROS_ERROR("[PluginManager] removePlugin: Call without name!");

  return false;
}

bool PluginManager::removePluginByName(const std::string& name)
{
  std::map<std::string, Plugin::Ptr>::iterator itr = Instance()->plugins_by_name.find(name);
  if (itr == Instance()->plugins_by_name.end())
    return false;

  ROS_INFO("[PluginManager] Removed plugin '%s' with type_id '%s'", itr->second->getName().c_str(), itr->second->getTypeId().c_str());
  Instance()->plugins_by_name.erase(itr);

  // publish update
  Instance()->publishPluginStateUpdate();

  return true;
}

void PluginManager::removePlugin(Plugin::Ptr& plugin)
{
  removePluginByName(plugin->getName());
}

void PluginManager::removePluginsByTypeId(const std::string& type_id)
{
  for (std::map<std::string, Plugin::Ptr>::iterator itr = Instance()->plugins_by_name.begin(); itr != Instance()->plugins_by_name.end();)
  {
    if (itr->second->getTypeId() == type_id)
      removePluginByName(itr++->first);
    else
      itr++;
  }
}

bool PluginManager::loadPluginSet(const std::vector<msgs::PluginDescription>& plugin_descriptions)
{
  bool success = true;

  // get list of active plugins
  std::vector<msgs::PluginDescription> active_plugins;
  for (std::map<std::string, Plugin::Ptr>::const_iterator itr = Instance()->plugins_by_name.begin(); itr != Instance()->plugins_by_name.end(); itr++)
    active_plugins.push_back(itr->second->getDescription());

  // remove all plugins which are not existing in list
  std::vector<msgs::PluginDescription> remove_plugin_list;
  vigir_pluginlib::filterDescriptionList(remove_plugin_list, active_plugins, plugin_descriptions, true);
  if (!removePlugins(remove_plugin_list))
    success = false;

  // add new plugins
  std::vector<msgs::PluginDescription> add_plugin_list;
  vigir_pluginlib::filterDescriptionList(add_plugin_list, plugin_descriptions, active_plugins, true);
  if (!addPlugins(add_plugin_list))
    success = false;

  return success;
}

bool PluginManager::loadPluginSet(const std::string& name)
{
  std::string prefix = "plugin_sets/" + name;

  if (!Instance()->nh.hasParam(prefix))
  {
    ROS_ERROR("[PluginManager] loadPluginSet: Couldn't find plugin set '%s' at parameter server.", name.c_str());
    return false;
  }

  ROS_INFO("[PluginManager] loadPluginSet: Loading plugin set '%s'...", name.c_str());

  // grab all plugin descriptions in the subtree
  XmlRpc::XmlRpcValue val;
  Instance()->nh.getParam(prefix, val);

  for (const auto& kv : val) {
    // TODO
  }

  return true;
}

bool PluginManager::hasPlugin(Plugin::Ptr& plugin)
{
  std::map<std::string, Plugin::Ptr>::iterator itr = Instance()->plugins_by_name.find(plugin->getName());
  return (itr != Instance()->plugins_by_name.end() && itr->second->getTypeId() == plugin->getTypeId());
}

bool PluginManager::hasPluginByName(const std::string& name)
{
  return Instance()->plugins_by_name.find(name) != Instance()->plugins_by_name.end();
}

bool PluginManager::hasPluginsByTypeId(const std::string& type_id)
{
  for (std::map<std::string, Plugin::Ptr>::iterator itr = Instance()->plugins_by_name.begin(); itr != Instance()->plugins_by_name.end(); itr++)
  {
    if (itr->second->getTypeId() == type_id)
      return true;
  }
}

void PluginManager::loadParams(const vigir_generic_params::ParameterSet& params)
{
  for (std::map<std::string, Plugin::Ptr>::iterator itr = Instance()->plugins_by_name.begin(); itr != Instance()->plugins_by_name.end(); itr++)
    itr->second->loadParams(params);
}

bool PluginManager::getPluginDescription(const std::string& key, msgs::PluginDescription& description)
{
  description = msgs::PluginDescription();

  if (!Instance()->nh.hasParam(key))
  {
    ROS_ERROR("[PluginManager] getPluginDescription: Couldn't retrieve plugin description at '%s' from parameter server.", key.c_str());
    return false;
  }

  Instance()->nh.getParam(key + "/name", description.name.data);
  Instance()->nh.getParam(key + "/type_class", description.type_class.data);
  Instance()->nh.getParam(key + "/type_class_package", description.type_class_package.data);
  Instance()->nh.getParam(key + "/base_class", description.base_class.data);
  Instance()->nh.getParam(key + "/base_class_package", description.base_class_package.data);

  return !description.name.data.empty() || !description.type_class.data.empty();
}

void PluginManager::publishPluginStateUpdate()
{
  msgs::PluginStates plugin_states;
  getPluginStates(plugin_states.states);
  plugin_states_pub.publish(plugin_states);
}

// --- Subscriber calls ---

void PluginManager::addPlugin(const msgs::PluginDescriptionConstPtr plugin_description)
{
  addPlugin(*plugin_description);
}

void PluginManager::removePlugin(const msgs::PluginDescriptionConstPtr plugin_description)
{
  removePlugin(*plugin_description);
}

// --- Service calls ---

bool PluginManager::getPluginDescriptionsService(msgs::GetPluginDescriptionsService::Request& req, msgs::GetPluginDescriptionsService::Response& resp)
{
  getPluginDescriptions(resp.descriptions, req.filter);
  return true;
}

bool PluginManager::getPluginStatesService(msgs::GetPluginStatesService::Request& req, msgs::GetPluginStatesService::Response& resp)
{
  getPluginStates(resp.states, req.filter);
  return true;
}

bool PluginManager::addPluginService(msgs::PluginManagementService::Request& req, msgs::PluginManagementService::Response& /*resp*/)
{
  return addPlugins(req.descriptions);
}

bool PluginManager::removePluginService(msgs::PluginManagementService::Request& req, msgs::PluginManagementService::Response& /*resp*/)
{
  return removePlugins(req.descriptions);
}

bool PluginManager::loadPluginSetService(msgs::PluginManagementService::Request& req, msgs::PluginManagementService::Response& /*resp*/)
{
  if (!req.name.data.empty())
    return loadPluginSet(req.name.data);
  else
    return loadPluginSet(req.descriptions);
}

// --- Action Server calls ---

void PluginManager::getPluginDescriptionsAction(const msgs::GetPluginDescriptionsGoalConstPtr goal)
{
  // check if new goal was preempted in the meantime
  if (get_plugin_descriptions_as->isPreemptRequested())
  {
    get_plugin_descriptions_as->setPreempted();
    return;
  }

  msgs::GetPluginDescriptionsResult result;
  getPluginDescriptions(result.descriptions, goal->filter);

  get_plugin_descriptions_as->setSucceeded(result);
}

void PluginManager::getPluginStatesAction(const msgs::GetPluginStatesGoalConstPtr goal)
{
  // check if new goal was preempted in the meantime
  if (get_plugin_states_as->isPreemptRequested())
  {
    get_plugin_states_as->setPreempted();
    return;
  }

  msgs::GetPluginStatesResult result;
  getPluginStates(result.states, goal->filter);

  get_plugin_states_as->setSucceeded(result);
}

void PluginManager::addPluginAction(const msgs::PluginManagementGoalConstPtr goal)
{
  // check if new goal was preempted in the meantime
  if (add_plugin_as->isPreemptRequested())
  {
    add_plugin_as->setPreempted();
    return;
  }

  msgs::PluginManagementResult result;
  result.success.data = addPlugins(goal->descriptions);

  if (result.success.data)
    add_plugin_as->setSucceeded(result);
  else
    add_plugin_as->setAborted(result);
}

void PluginManager::removePluginAction(const msgs::PluginManagementGoalConstPtr goal)
{
  // check if new goal was preempted in the meantime
  if (remove_plugin_as->isPreemptRequested())
  {
    remove_plugin_as->setPreempted();
    return;
  }

  msgs::PluginManagementResult result;
  result.success.data = removePlugins(goal->descriptions);

  if (result.success.data)
    remove_plugin_as->setSucceeded(result);
  else
    remove_plugin_as->setAborted(result);
}

void PluginManager::loadPluginSetAction(const msgs::PluginManagementGoalConstPtr goal)
{
  // check if new goal was preempted in the meantime
  if (load_plugin_set_as->isPreemptRequested())
  {
    load_plugin_set_as->setPreempted();
    return;
  }

  msgs::PluginManagementResult result;
  if (!goal->name.data.empty())
    result.success.data = loadPluginSet(goal->name.data);
  else
    result.success.data = loadPluginSet(goal->descriptions);

  if (result.success.data)
    load_plugin_set_as->setSucceeded(result);
  else
    load_plugin_set_as->setAborted(result);
}
}
