/* +---------------------------------------------------------------------------+
   |                     Mobile Robot Programming Toolkit (MRPT)               |
   |                          http://www.mrpt.org/                             |
   |                                                                           |
   | Copyright (c) 2005-2016, Individual contributors, see AUTHORS file        |
   | See: http://www.mrpt.org/Authors - All rights reserved.                   |
   | Released under BSD License. See details in http://www.mrpt.org/License    |
   +---------------------------------------------------------------------------+ */

#ifndef CICPGoodnessERD_IMPL_H
#define CICPGoodnessERD_IMPL_H


using namespace mrpt::graphslam::deciders;

// Ctors, Dtors
// //////////////////////////////////

template<class GRAPH_t>
CICPGoodnessERD_t<GRAPH_t>::CICPGoodnessERD_t():
	params(*this), // pass reference to self when initializing the parameters
	m_search_disk_color(142, 142, 56),
	m_laser_scans_color(0, 20, 255),
	m_consecutive_invalid_format_instances_thres(20) // large threshold just to make sure
{
	MRPT_START;

	this->initCICPGoodnessERD_t();

	MRPT_END;
}
template<class GRAPH_t>
void CICPGoodnessERD_t<GRAPH_t>::initCICPGoodnessERD_t() {
	MRPT_START;

	m_win = NULL;
	m_win_manager = NULL;
	m_graph = NULL;

	m_initialized_visuals = false;
	m_initialized_rgbd_viewports = false;
	m_just_inserted_loop_closure = false;
	m_is_using_3DScan = false;

	// start ICP constraint registration only when 
	// nodeCount > m_last_total_num_of_nodes
	m_last_total_num_of_nodes = 2;

	m_edge_types_to_nums["ICP2D"] = 0;
	m_edge_types_to_nums["ICP3D"] = 0;
	m_edge_types_to_nums["LC"] = 0;

	m_checked_for_usuable_dataset = false;
	m_consecutive_invalid_format_instances = 0;

	std::cout << "CCICPGoodnessERD: Initialized class object" << std::endl;

	MRPT_END;
}
template<class GRAPH_t>
CICPGoodnessERD_t<GRAPH_t>::~CICPGoodnessERD_t() { }

// Method implementations
// //////////////////////////////////

template<class GRAPH_t> void CICPGoodnessERD_t<GRAPH_t>::updateDeciderState(
		mrpt::obs::CActionCollectionPtr action,
		mrpt::obs::CSensoryFramePtr observations,
		mrpt::obs::CObservationPtr observation ) {
	MRPT_START;
	MRPT_UNUSED_PARAM(action);
	
	// check possible prior node registration
	bool registered_new_node = false;

	if (m_last_total_num_of_nodes < m_graph->nodeCount()) {
		registered_new_node = true;
		m_last_total_num_of_nodes = m_graph->nodeCount();
		//std::cout << "[CICPGoodnessERD:] Registered new node. " << std::endl;
	}

	if (observation.present()) { // observation-only rawlog format
		if (IS_CLASS(observation, CObservation2DRangeScan)) {
			m_last_laser_scan2D =
				static_cast<mrpt::obs::CObservation2DRangeScanPtr>(observation);


			m_is_using_3DScan = false;
		}
		if (IS_CLASS(observation, CObservation3DRangeScan)) {
			m_last_laser_scan3D =
				static_cast<mrpt::obs::CObservation3DRangeScanPtr>(observation);
			// just load the range/intensity images - CGraphSlanEngine takes care
			// of the path
			m_last_laser_scan3D->load();

			// grab fake 2D range scan for visualization
			this->convert3DTo2DRangeScan(
					/*from = */ m_last_laser_scan3D,
					/*to   = */ &m_fake_laser_scan2D);


			m_is_using_3DScan = true;
		}

		// New node has been registered.
		// add the last laser_scan
		if (registered_new_node) {
			if (!m_last_laser_scan2D.null()) {
				m_nodes_to_laser_scans2D[m_graph->nodeCount()-1] = m_last_laser_scan2D;
				std::cout << "[CICPGoodnessERD:] Added laser scans of nodeID: "
					<< m_graph->nodeCount()-1 << std::endl;
			}
			if (!m_last_laser_scan3D.null()) {
				m_nodes_to_laser_scans3D[m_graph->nodeCount()-1] = m_last_laser_scan3D;
				std::cout << "[CICPGoodnessERD:] Added laser scans of nodeID: "
					<< m_graph->nodeCount()-1 << std::endl;
			}
		}
	}
	else { // action-observations rawlog format
		// append current laser scan
		m_last_laser_scan2D =
			observations->getObservationByClass<CObservation2DRangeScan>();
		if (registered_new_node && m_last_laser_scan2D) {
			m_nodes_to_laser_scans2D[m_graph->nodeCount()-1] = m_last_laser_scan2D;
			//std::cout << "Added laser scans of nodeID: "
			//<< m_graph->nodeCount()-1 << std::endl;
		}
	}

	// edge registration procedure - same for both rawlog formats
	if (registered_new_node) {
		// get set of nodes within predefined distance for ICP
		std::set<mrpt::utils::TNodeID> nodes_to_check_ICP;
		this->getNearbyNodesOf(
				&nodes_to_check_ICP,
				m_graph->nodeCount()-1,
				params.ICP_max_distance);
		std::cout << "Found * " << nodes_to_check_ICP.size() 
			<< " * nodes close to nodeID: " << m_graph->nodeCount()-1 << endl;

		// reset the loop_closure flag and run registration
		m_just_inserted_loop_closure = false;
		registered_new_node = false;

		if (m_is_using_3DScan) {
			checkRegistrationCondition3D(nodes_to_check_ICP);
		}
		else {
			checkRegistrationCondition2D(nodes_to_check_ICP);
		}
	}

	if (!m_checked_for_usuable_dataset) {
		this->checkIfInvalidDataset(action, observations, observation);
	}

	MRPT_END;
}

template<class GRAPH_t>
void CICPGoodnessERD_t<GRAPH_t>::checkRegistrationCondition2D(
		const std::set<mrpt::utils::TNodeID>& nodes_set) {
	MRPT_START;

	//cout << "CICPGoodnessERD: Checking 2D Registration Condition... " << endl;

  CObservation2DRangeScanPtr curr_laser_scan;
  std::map<const mrpt::utils::TNodeID, 
    mrpt::obs::CObservation2DRangeScanPtr>::const_iterator search;
	// search for curr_laser_scan
  search = m_nodes_to_laser_scans2D.find(m_graph->nodeCount()-1); 
  if (search != m_nodes_to_laser_scans2D.end()) {
    curr_laser_scan = search->second;
  }

	// commence only if I have the current laser scan
  if (curr_laser_scan) {
		// try adding ICP constraints with each node in the previous set
		for (set<mrpt::utils::TNodeID>::const_iterator 
				node_it = nodes_set.begin();
				node_it != nodes_set.end(); ++node_it) {

			// get the ICP edge between current and last node
			constraint_t rel_edge;
			mrpt::slam::CICP::TReturnInfo icp_info;
    	CObservation2DRangeScanPtr prev_laser_scan;

    	// search for prev_laser_scan
			search = m_nodes_to_laser_scans2D.find(*node_it); 
    	if (search != m_nodes_to_laser_scans2D.end()) {
        prev_laser_scan = search->second;

				//  TODO - use initial edge estimation
				this->getICPEdge(
						*prev_laser_scan,
						*curr_laser_scan,
						&rel_edge,
						NULL,
						&icp_info);

				// criterion for registering a new node
				if (icp_info.goodness > params.ICP_goodness_thresh) {
					this->registerNewEdge(*node_it, m_graph->nodeCount()-1, rel_edge);
					m_edge_types_to_nums["ICP2D"]++;
					// in case of loop closure
					if (abs(m_graph->nodeCount()-1 - *node_it) > params.LC_min_nodeid_diff) {
						m_edge_types_to_nums["LC"]++;
						m_just_inserted_loop_closure = true;
					}
				}
			}
		}
	}

	MRPT_END;
}
template<class GRAPH_t>
void CICPGoodnessERD_t<GRAPH_t>::checkRegistrationCondition3D(
		const std::set<mrpt::utils::TNodeID>& nodes_set) {
	MRPT_START;

  CObservation3DRangeScanPtr curr_laser_scan;
  std::map<const mrpt::utils::TNodeID, 
    mrpt::obs::CObservation3DRangeScanPtr>::const_iterator search;
	// search for curr_laser_scan
  search = m_nodes_to_laser_scans3D.find(m_graph->nodeCount()-1); 
  if (search != m_nodes_to_laser_scans3D.end()) {
    curr_laser_scan = search->second;
  }

	// commence only if I have the current laser scan
  if (curr_laser_scan) {
		// try adding ICP constraints with each node in the previous set
		for (set<mrpt::utils::TNodeID>::const_iterator 
				node_it = nodes_set.begin();
				node_it != nodes_set.end(); ++node_it) {

			// get the ICP edge between current and last node
			constraint_t rel_edge;
			mrpt::slam::CICP::TReturnInfo icp_info;
    	CObservation3DRangeScanPtr prev_laser_scan;

    	// search for prev_laser_scan
			search = m_nodes_to_laser_scans3D.find(*node_it); 
    	if (search != m_nodes_to_laser_scans3D.end()) {
        prev_laser_scan = search->second;

				// TODO - use initial edge estimation
				this->getICPEdge(
						*prev_laser_scan,
						*curr_laser_scan,
						&rel_edge,
						NULL,
						&icp_info);

				// criterion for registering a new node
				if (icp_info.goodness > params.ICP_goodness_thresh) {
					this->registerNewEdge(*node_it, m_graph->nodeCount()-1, rel_edge);
					m_edge_types_to_nums["ICP3D"]++;
					// in case of loop closure
					if (abs(m_graph->nodeCount()-1 - *node_it) > params.LC_min_nodeid_diff) {
						m_edge_types_to_nums["LC"]++;
						m_just_inserted_loop_closure = true;
					}
				}
			}
		}
	}

	MRPT_END;
}

template<class GRAPH_t>
void CICPGoodnessERD_t<GRAPH_t>::registerNewEdge(
    const mrpt::utils::TNodeID& from, 
    const mrpt::utils::TNodeID& to,
    const constraint_t& rel_edge ) {
  MRPT_START;

  //cout << "[CICPGoodnessERD:] Inserting new Edge: " << from << " -> " << to << endl;
	m_graph->insertEdge(from,  to, rel_edge);

	MRPT_END;
}

template<class GRAPH_t>
void CICPGoodnessERD_t<GRAPH_t>::getNearbyNodesOf(
		set<TNodeID> *nodes_set,
		const TNodeID& cur_nodeID,
		double distance ) {
	MRPT_START;

	if (distance > 0) {
		// check all but the last node.
		for (TNodeID nodeID = 0; nodeID < m_graph->nodeCount()-1; ++nodeID) {
			double curr_distance = m_graph->nodes[nodeID].distanceTo(
					m_graph->nodes[cur_nodeID]);
			//std::cout << "testing against node: " << nodeID << std::endl;
			//std::cout << "\tcurr_distance: " << curr_distance << std::endl;
			if (curr_distance <= distance) {
				nodes_set->insert(nodeID);
			}
		}
	}
	else { // check against all nodes 
		m_graph->getAllNodes(*nodes_set);
	}

	MRPT_END;
}


template<class GRAPH_t>
void CICPGoodnessERD_t<GRAPH_t>::setGraphPtr(GRAPH_t* graph) {
	MRPT_START;

	m_graph = graph;

	std::cout << "[CICPGoodnessERD:] Fetched the graph successfully" 
		<< std::endl;

	MRPT_END;
}
template<class GRAPH_t>
void CICPGoodnessERD_t<GRAPH_t>::setRawlogFname(const std::string& rawlog_fname){
	MRPT_START;

	m_rawlog_fname = rawlog_fname;
	std::cout << "[CICPGoodnessERD:] Fetched the rawlog filename successfully: "
		<< m_rawlog_fname << std::endl;

	// find the directory of the 3Dscan images in case we are working with
	// Cobservation3DRangeScans
	cout << "Trying to fetch 3D scans external storage directory.. " << endl;
	std::string rawlog_fname_noext = system::extractFileName(m_rawlog_fname);
	std::string rawlog_dir = system::extractFileDirectory(rawlog_fname);
	std::string img_external_storage_dir = 
		rawlog_dir + rawlog_fname_noext + "_Images/";

	if (system::directoryExists(img_external_storage_dir)) {
		params.scans_img_external_dir = img_external_storage_dir;
		cout << "3D scans external storage: " << params.scans_img_external_dir 
			<< endl;
	}
	else {
		cout << "Couldn't find 3D scans external storage: " << img_external_storage_dir << endl;
	}




	MRPT_END;
}
template<class GRAPH_t>
void CICPGoodnessERD_t<GRAPH_t>::setWindowManagerPtr(
		mrpt::gui::CWindowManager_t* win_manager) {
	m_win_manager = win_manager;
}
template<class GRAPH_t> void
CICPGoodnessERD_t<GRAPH_t>::setCDisplayWindowPtr(
		mrpt::gui::CDisplayWindow3D* win) {
	MRPT_START;

	m_win = win;

	std::cout << "[CICPGoodnessERD:] Fetched the CDisplayWindow successfully" 
		<< std::endl;

	MRPT_END;
}
template<class GRAPH_t>
void CICPGoodnessERD_t<GRAPH_t>::getEdgesStats(
		std::map<const std::string, int>* edge_types_to_nums) {
	MRPT_START;

	*edge_types_to_nums = m_edge_types_to_nums;

	MRPT_END;
}

template<class GRAPH_t>
void CICPGoodnessERD_t<GRAPH_t>::initializeVisuals() {
	MRPT_START;
	ASSERTMSG_(params.has_read_config,
			"[CICPGoodnessERD:] Configuration parameters aren't loaded yet");
	std::cout << "Initializing CICPGoodnessERD visuals" << std::endl;

	// ICP_max_distance disk
	if (m_win &&  params.ICP_max_distance > 0) {
		COpenGLScenePtr scene = m_win->get3DSceneAndLock();

		CDiskPtr obj = CDisk::Create();
		pose_t initial_pose;
		obj->setPose(initial_pose);
		obj->setName("ICP_max_distance");
		obj->setColor_u8(m_search_disk_color);
		obj->setDiskRadius(params.ICP_max_distance, params.ICP_max_distance-0.5);
		scene->insert(obj);

		m_win->unlockAccess3DScene();
		m_win->forceRepaint();
	}

	// laser scan visualization
	if (params.visualize_laser_scans) {
		COpenGLScenePtr scene = m_win->get3DSceneAndLock();

		CPlanarLaserScanPtr laser_scan_viz = mrpt::opengl::CPlanarLaserScan::Create();
		laser_scan_viz->enablePoints(true);
		laser_scan_viz->enableLine(true);
		laser_scan_viz->enableSurface(true);
		laser_scan_viz->setSurfaceColor(
				m_laser_scans_color.R,
				m_laser_scans_color.G,
				m_laser_scans_color.B,
				m_laser_scans_color.A);

		laser_scan_viz->setName("laser_scan_viz");

		scene->insert(laser_scan_viz);
		m_win->unlockAccess3DScene();
		m_win->forceRepaint();
	}

	// max distance disk - textMessage
	if (m_win && m_win_manager && params.ICP_max_distance > 0) {
		m_win_manager->assignTextMessageParameters(
				&m_offset_y_search_disk,
				&m_text_index_search_disk);

		m_win_manager->addTextMessage(5,-m_offset_y_search_disk,
				format("ICP Edges search radius"),
				mrpt::utils::TColorf(m_search_disk_color),
				/* unique_index = */ m_text_index_search_disk );
	}

	m_initialized_visuals = true;
	MRPT_END;
}
template<class GRAPH_t>
void CICPGoodnessERD_t<GRAPH_t>::updateVisuals() {
	MRPT_START;
	ASSERT_(m_initialized_visuals);
	//std::cout << "Updating CICPGoodnessERD visuals" << std::endl;

	// update ICP_max_distance Disk
	if (m_win && params.ICP_max_distance > 0) {
		COpenGLScenePtr scene = m_win->get3DSceneAndLock();

		CRenderizablePtr obj = scene->getByName("ICP_max_distance");
		CDiskPtr disk_obj = static_cast<CDiskPtr>(obj);

		disk_obj->setPose(m_graph->nodes[m_graph->nodeCount()-1]);

		m_win->unlockAccess3DScene();
		m_win->forceRepaint();
	}

	// update laser scan visual
	if (params.visualize_laser_scans && (!m_last_laser_scan2D.null() || !m_fake_laser_scan2D.null())) {
		COpenGLScenePtr scene = m_win->get3DSceneAndLock();

		CRenderizablePtr obj = scene->getByName("laser_scan_viz");
		CPlanarLaserScanPtr laser_scan_viz = static_cast<CPlanarLaserScanPtr>(obj);

		// if fake 2D exists use it
		if (!m_fake_laser_scan2D.null()) {
			// set the scan contents
			laser_scan_viz->setScan(*m_fake_laser_scan2D);
		}
		else {
			laser_scan_viz->setScan(*m_last_laser_scan2D);
		}

		// set the pose of the laser scan
		typename GRAPH_t::global_poses_t::const_iterator search = 
			m_graph->nodes.find(m_graph->nodeCount()-1);
		if (search != m_graph->nodes.end()) {
			laser_scan_viz->setPose(m_graph->nodes[m_graph->nodeCount()-1]);
			// put the laser scan underneath the graph, so that you can still
			// visualize the loop closures with the nodes ahead
			laser_scan_viz->setPose(CPose3D(
						laser_scan_viz->getPoseX(), laser_scan_viz->getPoseY(), -0.5,
						DEG2RAD(laser_scan_viz->getPoseYaw()), 
						DEG2RAD(laser_scan_viz->getPosePitch()), 
						DEG2RAD(laser_scan_viz->getPoseRoll())
						));
		}

		m_win->unlockAccess3DScene();
		m_win->forceRepaint();
	}

	// RGB image visual
	if (m_is_using_3DScan) {
		// initialize the viewport if not there
		if (!m_initialized_rgbd_viewports) {
			cout << "Initializing the RGBD viewports..." << endl;

			// intensity viewport
			if (params.enable_intensity_viewport) {
				COpenGLScenePtr scene = m_win->get3DSceneAndLock();
				opengl::COpenGLViewportPtr viewp_intensity;

				viewp_intensity = scene->createViewport("viewp_intensity");
				// TODO - assign position using window_manager
				viewp_intensity->setViewportPosition(0.78,0.56,0.20,0.20);

				m_win->unlockAccess3DScene();
				m_win->forceRepaint();
			}

			// range viewport
			if (params.enable_range_viewport) {
				COpenGLScenePtr scene = m_win->get3DSceneAndLock();
				opengl::COpenGLViewportPtr viewp_range;

				viewp_range = scene->createViewport("viewp_range");
				// TODO - assign position using window_manager
				viewp_range->setViewportPosition(0.78,0.34,0.20,0.20);

				m_win->unlockAccess3DScene();
				m_win->forceRepaint();
			}

			m_initialized_rgbd_viewports = true;
		}

		// in either case update them..
		// Show intensity image:
		if (m_last_laser_scan3D->hasIntensityImage && params.enable_intensity_viewport) {
			mrpt::utils::CImage img  = m_last_laser_scan3D->intensityImage;

			COpenGLScenePtr scene = m_win->get3DSceneAndLock();
			COpenGLViewportPtr viewp_intensity = scene->getViewport("viewp_intensity");
			//viewp_intensity->setImageView_fast(m_last_laser_scan3D->intensityImage);
			viewp_intensity->setImageView_fast(img);
			m_win->unlockAccess3DScene();
			m_win->forceRepaint();
		}

		// show the range image
		if (m_last_laser_scan3D->hasRangeImage && params.enable_range_viewport) {

			// make this a static class member
			CMatrixFloat range2D;
			mrpt::utils::CImage img;
	
			range2D = m_last_laser_scan3D->rangeImage * (1.0/5.0); // TODO - without the magic number?
			img.setFromMatrix(range2D);

			COpenGLScenePtr scene = m_win->get3DSceneAndLock();
			COpenGLViewportPtr viewp_range = scene->getViewport("viewp_range");
			viewp_range->setImageView_fast(img);
			m_win->unlockAccess3DScene();
			m_win->forceRepaint();
		}

	}
	MRPT_END;
}
template<class GRAPH_t>
bool CICPGoodnessERD_t<GRAPH_t>::justInsertedLoopClosure() {
	return m_just_inserted_loop_closure;
}

template<class GRAPH_t>
void CICPGoodnessERD_t<GRAPH_t>::checkIfInvalidDataset(
		mrpt::obs::CActionCollectionPtr action,
		mrpt::obs::CSensoryFramePtr observations,
		mrpt::obs::CObservationPtr observation ) {
	MRPT_START;
	MRPT_UNUSED_PARAM(action);

	if (observation.present()) { // FORMAT #2
		if (IS_CLASS(observation, CObservation2DRangeScan) || 
				IS_CLASS(observation, CObservation3DRangeScan)) {
			m_checked_for_usuable_dataset = true;
			return;
		}
		else {
			m_consecutive_invalid_format_instances++;
		}
	}
	else {
		// TODO - what if it's in this format but only has odometry information?
		m_checked_for_usuable_dataset = true;
		return;
	}
	if (m_consecutive_invalid_format_instances > m_consecutive_invalid_format_instances_thres) {
		std::cout << "[CICPGoodnessERD:] Can't find usuable data in the given dataset." 
			<< std::endl;
		std::cout << "Make sure dataset contains valid CObservation2DRangeScan/CObservation3DRangeScan data." 
			<< std::endl;
		mrpt::system::sleep(5000);
		m_checked_for_usuable_dataset = true;
	}

	MRPT_END;
}



// TODO - remove this
template<class GRAPH_t>
void CICPGoodnessERD_t<GRAPH_t>::correct3DScanImageFname(
		mrpt::utils::CImage* img,
		std::string extension /*= ".png" */ ) {

	if (!params.scans_img_external_dir.empty()) {
		// fetch the correct absolute path of the image
		string relative_fname = img->getExternalStorageFile();
		string fname = system::extractFileName(relative_fname);
		fname = params.scans_img_external_dir + fname + extension;

		// set the image file path appropriately
		img->setExternalStorage(fname);
	}
}

// TParameter
// //////////////////////////////////

template<class GRAPH_t>
CICPGoodnessERD_t<GRAPH_t>::TParams::TParams(decider_t& d):
	decider(d),
	has_read_config(false)
{ }

template<class GRAPH_t>
CICPGoodnessERD_t<GRAPH_t>::TParams::~TParams() {
}

template<class GRAPH_t>
void CICPGoodnessERD_t<GRAPH_t>::TParams::dumpToTextStream(
		mrpt::utils::CStream &out) const {
	MRPT_START;

	out.printf("------------------[ Goodness-based ICP Edge Registration ]------------------\n");
	out.printf("ICP goodness threshold         = %.2f%% \n", ICP_goodness_thresh*100);
	out.printf("ICP max radius for edge search = %.2f\n", ICP_max_distance);
	out.printf("Min. node difference for LC    = %d\n", LC_min_nodeid_diff);
	out.printf("Visualize laser scans          = %d\n", visualize_laser_scans);
	out.printf("Enable intensity img viewport  = %d\n", enable_intensity_viewport);
	out.printf("Enable range img viewport      = %d\n", enable_range_viewport);
	out.printf("3DScans Image Directory        = %s\n", scans_img_external_dir.c_str());

	decider.range_scanner_t::params.dumpToTextStream(out);

	MRPT_END;
}
template<class GRAPH_t>
void CICPGoodnessERD_t<GRAPH_t>::TParams::loadFromConfigFile(
		const mrpt::utils::CConfigFileBase& source,
    const std::string& section) {
  MRPT_START;

	ICP_max_distance = source.read_double(
			section,
			"ICP_max_distance",
			10, false);
	ICP_goodness_thresh = source.read_double(
			section,
			"ICP_goodness_thresh",
	 		0.75, false);
  LC_min_nodeid_diff = source.read_int(
  		section,
 			"LC_min_nodeid_diff",
 			10, false);
  visualize_laser_scans = source.read_bool(
  		"VisualizationParameters",
 			"visualize_laser_scans",
 			true, false);
  enable_intensity_viewport= source.read_bool(
  		"VisualizationParameters",
 			"enable_intensity_viewport",
 			true, false);
  enable_range_viewport = source.read_bool(
  		"VisualizationParameters",
 			"enable_range_viewport",
 			true, false);
 	scans_img_external_dir = source.read_string(
 			section,
 			"scan_images_external_directory",
 			"", false);

	// load the icp parameters - from "ICP" section explicitly
	decider.range_scanner_t::params.loadFromConfigFile(source, "ICP");

	std::cout << "[CICPGoodnessERD:] Successfully loaded CICPGoodnessERD parameters. " 
		<< std::endl;
	has_read_config = true;

	MRPT_END;
}


#endif /* end of include guard: CICPGoodnessERD_IMPL_H */
