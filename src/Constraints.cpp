#include "Constraints.h"

#ifdef EOLC_ONLINE
#define GLEW_STATIC
#include <GL/glew.h>
#include <GLFW/glfw3.h>

#define GLM_FORCE_RADIANS
#include <glm/glm.hpp>
#include <glm/gtc/type_ptr.hpp>
#include "glm/ext.hpp"

#include "online/Program.h"
#include "online/MatrixStack.h"
#endif // EOLC_ONLINE

#include "Obstacles.h"
#include "Box.h"
#include "Points.h"
#include "Rigid.h"
#include "Collisions.h"
#include "FixedList.h"
#include "conversions.h"
//#include "external\ArcSim\mesh.hpp"
#include "external/ArcSim/util.hpp"
#include "external/ArcSim/geometry.hpp"

#include <iostream>
#include <utility>

using namespace std;
using namespace Eigen;

Constraints::Constraints() :
	hasFixed(false),
	hasCollisions(false)
{

}

void Constraints::init(const shared_ptr<Obstacles> obs)
{
	int total_size = obs->points->num_points;
	for (int b = 0; b < obs->boxes.size(); b++) {
		total_size += (obs->boxes[b]->num_points + obs->boxes[b]->num_edges);
	}

	constraintTable.resize(9,total_size);
	constraintTable.setZero();

	for (int p = 0; p < obs->points->num_points; p++) {
		constraintTable.block(0, p, 3, 1) = obs->points->norms.col(p);
	}

	for (int b = 0; b < obs->boxes.size(); b++) {
		for (int p = 0; p < obs->boxes[b]->num_points; p++) {
			int index = obs->points->num_points + (b* obs->boxes[b]->num_points) + (b* obs->boxes[b]->num_edges) + p;
			Vector3d corner_nor = Vector3d::Zero();
			for (int i = 0; i < 3; i++) {
				for (int j = 0; j < 2; j++) {
					corner_nor += obs->boxes[b]->faceNorms.col(obs->boxes[b]->edgeFaces(j, obs->boxes[b]->vertEdges1(i, p)));
				}
			}
			corner_nor /= 6.0;
			constraintTable.block(0, index, 3, 1) = corner_nor.normalized();

		}
		for (int e = 0; e < obs->boxes[b]->num_edges; e++) {
			int index = obs->points->num_points + (b* obs->boxes[b]->num_points) + (b* obs->boxes[b]->num_edges) + (obs->boxes[b]->num_points + e);
			constraintTable.block(0, index, 3, 1) = obs->boxes[b]->faceNorms.col(obs->boxes[b]->edgeFaces(0, e));
			constraintTable.block(3, index, 3, 1) = obs->boxes[b]->faceNorms.col(obs->boxes[b]->edgeFaces(1, e));
			constraintTable.block(6, index, 3, 1) = obs->boxes[b]->faceNorms.col(obs->boxes[b]->edgeTan(e));
		}
	}
}

void Constraints::updateTable(const shared_ptr<Obstacles> obs)
{
	int total_size = obs->points->num_points;
	for (int b = 0; b < obs->boxes.size(); b++) {
		total_size += (obs->boxes[b]->num_points + obs->boxes[b]->num_edges);
	}

	constraintTable.resize(9,total_size);

	for (int p = 0; p < obs->points->num_points; p++) {
		constraintTable.block(0, p, 3, 1) = obs->points->norms.col(p);
	}

	for (int b = 0; b < obs->boxes.size(); b++) {
		for (int p = 0; p < obs->boxes[b]->num_points; p++) {
			int index = obs->points->num_points + (b* obs->boxes[b]->num_points) + (b* obs->boxes[b]->num_edges) + p;
			Vector3d corner_nor = Vector3d::Zero();
			for (int i = 0; i < 3; i++) {
				for (int j = 0; j < 2; j++) {
					corner_nor += obs->boxes[b]->faceNorms.col(obs->boxes[b]->edgeFaces(j, obs->boxes[b]->vertEdges1(i, p)));
				}
			}
			corner_nor /= 6.0;
			constraintTable.block(0, index, 3, 1) = corner_nor.normalized();

		}
		for (int e = 0; e < obs->boxes[b]->num_edges; e++) {
			int index = obs->points->num_points + (b* obs->boxes[b]->num_points) + (b* obs->boxes[b]->num_edges) + (obs->boxes[b]->num_points + e);
			constraintTable.block(0, index, 3, 1) = obs->boxes[b]->faceNorms.col(obs->boxes[b]->edgeFaces(0, e));
			constraintTable.block(3, index, 3, 1) = obs->boxes[b]->faceNorms.col(obs->boxes[b]->edgeFaces(1, e));
			constraintTable.block(6, index, 3, 1) = obs->boxes[b]->faceNorms.col(obs->boxes[b]->edgeTan(e));
		}
	}
}

typedef Eigen::Triplet<double> T;

void addFixed(const VectorXd& c, vector<T>& Aeq_, vector< pair<int, double> >& beq_, const double& v, const int& ci, const int& i, int& eqsize)
{
	Aeq_.push_back(T(eqsize, ci, c(i)));
	beq_.push_back(make_pair(eqsize, (1 - 0.01) * v + c(i + 3)));
	eqsize++;
}

// TODO:: This can probably be split into three nicer looking functions
void Constraints::fill(const Mesh& mesh, const shared_ptr<Obstacles> obs, const shared_ptr<FixedList> fs, double h, const bool& online)
{
	updateTable(obs);

	hasFixed = false;
	hasCollisions = false;

	vector<T> Aeq_;
	vector<T> Aineq_;
	vector< pair<int, double> > beq_;
	vector< pair<int, double> > bineq_;
	//vector<T> beq_;
	//vector<T> bineq_;

	if (online) {
		drawAineq.clear();
		drawAeq.clear();
	}

	int eqsize = 0;
	int ineqsize = 0;

	// We need to do some rigid body calculations for moving collisions
	shared_ptr<Rigid> rigid;

	for (int n = 0; n < mesh.nodes.size(); n++) {
		if (mesh.nodes[n]->EoL) {
			if (mesh.nodes[n]->cornerID >= 0) {
				Vector3d nor = constraintTable.block(0, mesh.nodes[n]->cornerID, 3, 1);
				Vector3d ortho1 = Vector3d(0.0, -nor(2), nor(1));
				Vector3d ortho2 = (ortho1.cross(nor)).normalized();
				ortho2.normalize();

				// Get the movement data for moving obects
				Vector4d xdot;
				double bfill;
				bool movement = false;
				// It's simple if we just have a point collision
				if (mesh.nodes[n]->cornerID < obs->points->num_points) {

				}
				// Box corners take more work
				else {
					// This will int arithmetic down to the box number since there are a combined 20 box corners and edges
					int boxnum = (mesh.nodes[n]->cornerID - obs->points->num_points) / 20.0;
					if (obs->boxes[boxnum]->v.segment(0,3).norm() != 0.0 || obs->boxes[boxnum]->v.segment(3, 3).norm() != 0.0) {
						movement = true;
						Vector4d xl = obs->boxes[boxnum]->E1inv*Vector4d(mesh.nodes[n]->x[0], mesh.nodes[n]->x[1], mesh.nodes[n]->x[2], 1.0);
						Matrix4d et = rigid->integrate(obs->boxes[boxnum]->E1, obs->boxes[boxnum]->v, h);
						xdot = ((et*xl) - (obs->boxes[boxnum]->E1*xl)) / h;
					}
				}

				// When locally flat we have a null space problem so we need to check
				bool flat = true;
				Node* node = mesh.nodes[n];
				Face* face0 = node->verts[0]->adjf[0];
				for (int f = 1; f < node->verts[0]->adjf.size(); f++) {
					Face* face1 = node->verts[0]->adjf[f];
					if (get_angle(face0->n, face1->n) > 0.5) {
						flat = false;
						break;
					}
				}

				// We want out orthonormal point constraints to be equality constraints if the mesh is locally flat or just a point and not a corner
				if (flat || node->cornerID < obs->points->num_points) {
					Aineq_.push_back(T(ineqsize, n * 3, -nor(0)));
					Aineq_.push_back(T(ineqsize, n * 3 + 1, -nor(1)));
					Aineq_.push_back(T(ineqsize, n * 3 + 2, -nor(2)));
					if (online) {
						drawAineq.push_back(Vector3d(ineqsize, mesh.nodes[n]->x[0], nor(0)));
						drawAineq.push_back(Vector3d(ineqsize, mesh.nodes[n]->x[1], nor(1)));
						drawAineq.push_back(Vector3d(ineqsize, mesh.nodes[n]->x[2], nor(2)));
					}
					if (movement) {
						bfill = nor.transpose() * xdot.segment<3>(0);
						bineq_.push_back(make_pair(ineqsize, -bfill));
					}
					ineqsize++;

					Aeq_.push_back(T(eqsize, n * 3, ortho1(0)));
					Aeq_.push_back(T(eqsize, n * 3 + 1, ortho1(1)));
					Aeq_.push_back(T(eqsize, n * 3 + 2, ortho1(2)));
					if (online) {
						drawAeq.push_back(Vector3d(eqsize, mesh.nodes[n]->x[0], ortho1(0)));
						drawAeq.push_back(Vector3d(eqsize, mesh.nodes[n]->x[1], ortho1(1)));
						drawAeq.push_back(Vector3d(eqsize, mesh.nodes[n]->x[2], ortho1(2)));
					}
					if (movement) {
						bfill = ortho1.transpose() * xdot.segment<3>(0);
						beq_.push_back(make_pair(eqsize, bfill));
					}
					eqsize++;

					Aeq_.push_back(T(eqsize, n * 3, ortho2(0)));
					Aeq_.push_back(T(eqsize, n * 3 + 1, ortho2(1)));
					Aeq_.push_back(T(eqsize, n * 3 + 2, ortho2(2)));
					if (online) {
						drawAeq.push_back(Vector3d(eqsize, mesh.nodes[n]->x[0], ortho2(0)));
						drawAeq.push_back(Vector3d(eqsize, mesh.nodes[n]->x[1], ortho2(1)));
						drawAeq.push_back(Vector3d(eqsize, mesh.nodes[n]->x[2], ortho2(2)));
					}
					if (movement) {
						bfill = ortho2.transpose() * xdot.segment<3>(0);
						beq_.push_back(make_pair(eqsize, bfill));
					}
					eqsize++;
				}
				else {
					Aineq_.push_back(T(ineqsize, n * 3, -nor(0)));
					Aineq_.push_back(T(ineqsize, n * 3 + 1, -nor(1)));
					Aineq_.push_back(T(ineqsize, n * 3 + 2, -nor(2)));
					if (online) {
						drawAineq.push_back(Vector3d(ineqsize, mesh.nodes[n]->x[0], nor(0)));
						drawAineq.push_back(Vector3d(ineqsize, mesh.nodes[n]->x[1], nor(1)));
						drawAineq.push_back(Vector3d(ineqsize, mesh.nodes[n]->x[2], nor(2)));
					}
					if (movement) {
						bfill = nor.transpose() * xdot.segment<3>(0);
						bineq_.push_back(make_pair(ineqsize, -bfill));
					}
					ineqsize++;

					Aineq_.push_back(T(ineqsize, n * 3, -ortho1(0)));
					Aineq_.push_back(T(ineqsize, n * 3 + 1, -ortho1(1)));
					Aineq_.push_back(T(ineqsize, n * 3 + 2, -ortho1(2)));
					if (online) {
						drawAineq.push_back(Vector3d(ineqsize, mesh.nodes[n]->x[0], ortho1(0)));
						drawAineq.push_back(Vector3d(ineqsize, mesh.nodes[n]->x[1], ortho1(1)));
						drawAineq.push_back(Vector3d(ineqsize, mesh.nodes[n]->x[2], ortho1(2)));
					}
					if (movement) {
						bfill = ortho1.transpose() * xdot.segment<3>(0);
						bineq_.push_back(make_pair(ineqsize, bfill));
					}
					ineqsize++;

					Aineq_.push_back(T(ineqsize, n * 3, -ortho2(0)));
					Aineq_.push_back(T(ineqsize, n * 3 + 1, -ortho2(1)));
					Aineq_.push_back(T(ineqsize, n * 3 + 2, -ortho2(2)));
					if (online) {
						drawAineq.push_back(Vector3d(ineqsize, mesh.nodes[n]->x[0], ortho2(0)));
						drawAineq.push_back(Vector3d(ineqsize, mesh.nodes[n]->x[1], ortho2(1)));
						drawAineq.push_back(Vector3d(ineqsize, mesh.nodes[n]->x[2], ortho2(2)));
					}
					if (movement) {
						bfill = ortho2.transpose() * xdot.segment<3>(0);
						bineq_.push_back(make_pair(ineqsize, bfill));
					}
					ineqsize++;
				}
			}
			else {
				Node* node = mesh.nodes[n];

				// Get the movement data for moving obects
				Vector4d xdot;
				double bfill;
				bool movement = false;
				// This will int arithmetic down to the box number since there are a combined 20 box corners and edges
				int boxnum = (node->cdEdges[0] - obs->points->num_points) / 20.0;
				if (obs->boxes[boxnum]->v.segment(0, 3).norm() != 0.0 || obs->boxes[boxnum]->v.segment(3, 3).norm() != 0.0) {
					movement = true;
					Vector4d xl = obs->boxes[boxnum]->E1inv*Vector4d(node->x[0], node->x[1], node->x[2], 1.0);
					Matrix4d et = rigid->integrate(obs->boxes[boxnum]->E1, obs->boxes[boxnum]->v, h);
					xdot = ((et*xl) - (obs->boxes[boxnum]->E1*xl)) / h;
				}

				// When locally flat we have a null space problem so we need to check
				bool flat = true;
				Face* face0 = node->verts[0]->adjf[0];
				for (int f = 1 ; f < node->verts[0]->adjf.size(); f++) {
					Face* face1 = node->verts[0]->adjf[f];
					if (get_angle(face0->n, face1->n) > 0.1) {
						flat = false;
						break;
					}
				}

				// If the point is locally flat then we want to use an equality constraint
				if (flat) {
					Aineq_.push_back(T(ineqsize, n * 3, -node->n[0]));
					Aineq_.push_back(T(ineqsize, n * 3 + 1, -node->n[1]));
					Aineq_.push_back(T(ineqsize, n * 3 + 2, -node->n[2]));
					if (online) {
						drawAineq.push_back(Vector3d(ineqsize, node->x[0], node->n[0]));
						drawAineq.push_back(Vector3d(ineqsize, node->x[1], node->n[1]));
						drawAineq.push_back(Vector3d(ineqsize, node->x[2], node->n[2]));
					}
					if (movement) {
						bfill = v2e(node->n).transpose() * xdot.segment<3>(0);
						bineq_.push_back(make_pair(ineqsize, -bfill));
					}
					ineqsize++;

					Vector3d flatConstraint = v2e(node->n).cross(constraintTable.block<3,1>(6, node->cdEdges[0]));

					Aeq_.push_back(T(eqsize, n * 3, -flatConstraint(0)));
					Aeq_.push_back(T(eqsize, n * 3 + 1, -flatConstraint(1)));
					Aeq_.push_back(T(eqsize, n * 3 + 2, -flatConstraint(2)));
					if (online) {
						drawAeq.push_back(Vector3d(eqsize, node->x[0], flatConstraint(0)));
						drawAeq.push_back(Vector3d(eqsize, node->x[1], flatConstraint(1)));
						drawAeq.push_back(Vector3d(eqsize, node->x[2], flatConstraint(2)));
					}
					if (movement) {
						bfill = flatConstraint.transpose() * xdot.segment<3>(0);
						beq_.push_back(make_pair(eqsize, -bfill));
					}
					eqsize++;
				}
				else {
					Aineq_.push_back(T(ineqsize, n * 3, -constraintTable(0, node->cdEdges[0])));
					Aineq_.push_back(T(ineqsize, n * 3 + 1, -constraintTable(1, node->cdEdges[0])));
					Aineq_.push_back(T(ineqsize, n * 3 + 2, -constraintTable(2, node->cdEdges[0])));
					if (online) {
						drawAineq.push_back(Vector3d(ineqsize, node->x[0], constraintTable(0, node->cdEdges[0])));
						drawAineq.push_back(Vector3d(ineqsize, node->x[1], constraintTable(1, node->cdEdges[0])));
						drawAineq.push_back(Vector3d(ineqsize, node->x[2], constraintTable(2, node->cdEdges[0])));
					}
					if (movement) {
						Vector3d nor = constraintTable.block(0, node->cdEdges[0], 3, 1); // Only works if stored for some reason
						bfill = nor.transpose() * xdot.segment<3>(0);
						bineq_.push_back(make_pair(ineqsize, -bfill));
					}
					ineqsize++;

					Aineq_.push_back(T(ineqsize, n * 3, -constraintTable(3, node->cdEdges[0])));
					Aineq_.push_back(T(ineqsize, n * 3 + 1, -constraintTable(4, node->cdEdges[0])));
					Aineq_.push_back(T(ineqsize, n * 3 + 2, -constraintTable(5, node->cdEdges[0])));
					if (online) {
						drawAineq.push_back(Vector3d(ineqsize, node->x[0], constraintTable(3, node->cdEdges[0])));
						drawAineq.push_back(Vector3d(ineqsize, node->x[1], constraintTable(4, node->cdEdges[0])));
						drawAineq.push_back(Vector3d(ineqsize, node->x[2], constraintTable(5, node->cdEdges[0])));
					}
					if (movement) {
						Vector3d nor = constraintTable.block(3, node->cdEdges[0], 3, 1); // Only works if stored for some reason
						bfill = nor.transpose() * xdot.segment<3>(0);
						bineq_.push_back(make_pair(ineqsize, -bfill));
					}
					ineqsize++;
				}

				// If a boundary, the Eulerian constraint stops it from moving outside
				if (is_seam_or_boundary(node)) {
					// Is this sufficient enough?
					Edge* edge;
					for (int e = 0; e < node->adje.size(); e++) {
						if (is_seam_or_boundary(node->adje[e])) {
							edge = node->adje[e];
							break;
						}
					}
					Node* opp_node = other_node(edge, node);
					Vector2d orth_border = Vector2d(node->verts[0]->u[1] - opp_node->verts[0]->u[1], -node->verts[0]->u[0] - opp_node->verts[0]->u[0]).normalized(); // This should be orthogonal to the edge connecting the two nodes
					Aeq_.push_back(T(eqsize, mesh.nodes.size() * 3 + node->EoL_index * 2, orth_border(0)));
					Aeq_.push_back(T(eqsize, mesh.nodes.size() * 3 + node->EoL_index * 2 + 1, orth_border(1)));
					if (online) {
						drawAeq.push_back(Vector3d(eqsize, node->x[0], orth_border(0)));
						drawAeq.push_back(Vector3d(eqsize, node->x[1], orth_border(1)));
						drawAeq.push_back(Vector3d(eqsize, 1.0, 0.0));
					}
					eqsize++;
				}
				// If internal, the Eulerian constraint forces tangential motion to realize in the Lagrangian space
				else {
					Vector2d tan_ave = Vector2d::Zero();
					int tot_conf = 0;
					for (int e = 0; e < node->adje.size(); e++) {
						if (node->adje[e]->preserve) {
							Edge* edge = node->adje[e];
							if (norm(edge->n[0]->verts[0]->u) > norm(edge->n[1]->verts[0]->u)) {
								tan_ave += Vector2d(edge->n[1]->verts[0]->u[0] - edge->n[0]->verts[0]->u[0], edge->n[1]->verts[0]->u[1] - edge->n[0]->verts[0]->u[1]).normalized();
							}
							else {
								tan_ave += Vector2d(edge->n[0]->verts[0]->u[0] - edge->n[1]->verts[0]->u[0], edge->n[0]->verts[0]->u[1] - edge->n[1]->verts[0]->u[1]).normalized();
							}
							tot_conf++;
						}
					}
					tan_ave.normalize();
					Aeq_.push_back(T(eqsize, mesh.nodes.size() * 3 + node->EoL_index * 2, tan_ave(0)));
					Aeq_.push_back(T(eqsize, mesh.nodes.size() * 3 + node->EoL_index * 2 + 1, tan_ave(1)));
					if (online) {
						drawAeq.push_back(Vector3d(eqsize, node->x[0], tan_ave(0)));
						drawAeq.push_back(Vector3d(eqsize, node->x[1], tan_ave(1)));
						drawAeq.push_back(Vector3d(eqsize, 1.0, 0.0));
					}
					eqsize++;
				}
			}
		}
	}

	// CD2
	// We use another collision detection step for 3 reasons
	// - We need to detect Cloth-vert to Box-face collisions post remeshing
	// - If we run a non EOL simulation we want our constraints to be based on remeshed geometry,
	// - We want to revert parts of EOL simulation to traditional LAG
	vector<shared_ptr<btc::Collision> > clsLAG;
	CD2(mesh, obs, clsLAG);
	for (int i = 0; i < clsLAG.size(); i++) {
		if (clsLAG[i]->count1 == 3 && clsLAG[i]->count2 == 1) {
			if (mesh.nodes[clsLAG[i]->verts2(0)]->EoL) continue;
			Aineq_.push_back(T(ineqsize, clsLAG[i]->verts2(0) * 3, -clsLAG[i]->nor1(0)));
			Aineq_.push_back(T(ineqsize, clsLAG[i]->verts2(0) * 3 + 1, -clsLAG[i]->nor1(1)));
			Aineq_.push_back(T(ineqsize, clsLAG[i]->verts2(0) * 3 + 2, -clsLAG[i]->nor1(2)));
			if (online) {
				drawAineq.push_back(Vector3d(ineqsize, clsLAG[i]->pos2(0), clsLAG[i]->nor1(0)));
				drawAineq.push_back(Vector3d(ineqsize, clsLAG[i]->pos2(1), clsLAG[i]->nor1(1)));
				drawAineq.push_back(Vector3d(ineqsize, clsLAG[i]->pos2(2), clsLAG[i]->nor1(2)));
			}
			ineqsize++;
		}
		else if (clsLAG[i]->count1 == 2 && clsLAG[i]->count2 == 2) {
			if (mesh.nodes[clsLAG[i]->verts2(0)]->EoL ||
				mesh.nodes[clsLAG[i]->verts2(1)]->EoL) continue;
			for (int j = 0; j < 2; j++) {
				Aineq_.push_back(T(ineqsize, clsLAG[i]->verts2(j) * 3, -clsLAG[i]->nor2(0) * clsLAG[i]->weights2(j)));
				Aineq_.push_back(T(ineqsize, clsLAG[i]->verts2(j) * 3 + 1, -clsLAG[i]->nor2(1) * clsLAG[i]->weights2(j)));
				Aineq_.push_back(T(ineqsize, clsLAG[i]->verts2(j) * 3 + 2, -clsLAG[i]->nor2(2) * clsLAG[i]->weights2(j)));
			}
			if (online) {
				drawAineq.push_back(Vector3d(ineqsize, clsLAG[i]->pos2(0), clsLAG[i]->nor1(0)));
				drawAineq.push_back(Vector3d(ineqsize, clsLAG[i]->pos2(1), clsLAG[i]->nor1(1)));
				drawAineq.push_back(Vector3d(ineqsize, clsLAG[i]->pos2(2), clsLAG[i]->nor1(2)));
			}
			ineqsize++;
		}
		else if (clsLAG[i]->count1 == 1 && clsLAG[i]->count2 == 3) {
			if (mesh.nodes[clsLAG[i]->verts2(0)]->EoL ||
				mesh.nodes[clsLAG[i]->verts2(1)]->EoL ||
				mesh.nodes[clsLAG[i]->verts2(2)]->EoL) continue;
			for (int j = 0; j < 3; j++) {
				Aineq_.push_back(T(ineqsize, clsLAG[i]->verts2(j) * 3, -clsLAG[i]->nor2(0) * clsLAG[i]->weights2(j)));
				Aineq_.push_back(T(ineqsize, clsLAG[i]->verts2(j) * 3 + 1, -clsLAG[i]->nor2(1) * clsLAG[i]->weights2(j)));
				Aineq_.push_back(T(ineqsize, clsLAG[i]->verts2(j) * 3 + 2, -clsLAG[i]->nor2(2) * clsLAG[i]->weights2(j)));
			}
			if (online) {
				drawAineq.push_back(Vector3d(ineqsize, clsLAG[i]->pos2(0), clsLAG[i]->nor1(0)));
				drawAineq.push_back(Vector3d(ineqsize, clsLAG[i]->pos2(1), clsLAG[i]->nor1(1)));
				drawAineq.push_back(Vector3d(ineqsize, clsLAG[i]->pos2(2), clsLAG[i]->nor1(2)));
			}
			ineqsize++;
		}
	}

	if (ineqsize > 0) hasCollisions = true;

	double expoFill = 0.01;
	if (fs->c1(0) != -1) {
		hasFixed = true;
		if (fs->c1(0) == 1.0) addFixed(fs->c1, Aeq_, beq_, mesh.nodes[fs->c1i]->v[0], fs->c1i * 3, 0, eqsize);
		if (fs->c1(1) == 1.0) addFixed(fs->c1, Aeq_, beq_, mesh.nodes[fs->c1i]->v[1], fs->c1i * 3 + 1, 1, eqsize);
		if (fs->c1(2) == 1.0) addFixed(fs->c1, Aeq_, beq_, mesh.nodes[fs->c1i]->v[2], fs->c1i * 3 + 2, 2, eqsize);
	}
	if (fs->c2(0) != -1) {
		hasFixed = true;
		if (fs->c2(0) == 1.0) addFixed(fs->c2, Aeq_, beq_, mesh.nodes[fs->c2i]->v[0], fs->c2i * 3, 0, eqsize);
		if (fs->c2(1) == 1.0) addFixed(fs->c2, Aeq_, beq_, mesh.nodes[fs->c2i]->v[1], fs->c2i * 3 + 1, 1, eqsize);
		if (fs->c2(2) == 1.0) addFixed(fs->c2, Aeq_, beq_, mesh.nodes[fs->c2i]->v[2], fs->c2i * 3 + 2, 2, eqsize);
	}
	if (fs->c3(0) != -1) {
		hasFixed = true;
		if (fs->c3(0) == 1.0) addFixed(fs->c3, Aeq_, beq_, mesh.nodes[fs->c3i]->v[0], fs->c3i * 3, 0, eqsize);
		if (fs->c3(1) == 1.0) addFixed(fs->c3, Aeq_, beq_, mesh.nodes[fs->c3i]->v[1], fs->c3i * 3 + 1, 1, eqsize);
		if (fs->c3(2) == 1.0) addFixed(fs->c3, Aeq_, beq_, mesh.nodes[fs->c3i]->v[2], fs->c3i * 3 + 2, 2, eqsize);
	}
	if (fs->c4(0) != -1) {
		hasFixed = true;
		if (fs->c4(0) == 1.0) addFixed(fs->c4, Aeq_, beq_, mesh.nodes[fs->c4i]->v[0], fs->c4i * 3, 0, eqsize);
		if (fs->c4(1) == 1.0) addFixed(fs->c4, Aeq_, beq_, mesh.nodes[fs->c4i]->v[1], fs->c4i * 3 + 1, 1, eqsize);
		if (fs->c4(2) == 1.0) addFixed(fs->c4, Aeq_, beq_, mesh.nodes[fs->c4i]->v[2], fs->c4i * 3 + 2, 2, eqsize);
	}

	Aeq.resize(eqsize, mesh.nodes.size() * 3 + mesh.EoL_Count * 2);
	Aineq.resize(ineqsize, mesh.nodes.size() * 3 + mesh.EoL_Count * 2);
	beq.resize(eqsize);
	bineq.resize(ineqsize);

	Aeq.setFromTriplets(Aeq_.begin(), Aeq_.end());
	Aineq.setFromTriplets(Aineq_.begin(), Aineq_.end());

	beq.setZero();
	bineq.setZero();
	for (int i = 0; i < beq_.size(); i++) {
		beq(beq_[i].first) = beq_[i].second;
	}
	for (int i = 0; i < bineq_.size(); i++) {
		bineq(bineq_[i].first) = bineq_[i].second;
	}
}

#ifdef EOLC_ONLINE

void Constraints::drawSimple(shared_ptr<MatrixStack> MV, const shared_ptr<Program> prog) const
{
	glColor3f(1.0f, 0.0f, 0.0f);

	int connum = -1, inerdex = 0;
	Vector3d tcon, tpos;
	for (int i = 0; i < drawAineq.size(); i++) {
		if (connum != drawAineq[i](0)) {
			connum = drawAineq[i](0);
			inerdex = 0;
		}
		//cout << drawAineq[i](1) << endl;
		tpos(inerdex) = drawAineq[i](1);
		tcon(inerdex) = drawAineq[i](2);
		inerdex++;
		if (inerdex > 2) {
			glBegin(GL_LINES);
			glVertex3f(tpos(0), tpos(1), tpos(2));
			glVertex3f(tpos(0) + tcon(0), tpos(1) + tcon(1), tpos(2) + tcon(2));
			glEnd();
		}
	}

	glColor3f(0.0f, 1.0f, 0.0f);

	connum = -1, inerdex = 0;
	for (int i = 0; i < drawAeq.size(); i++) {
		if (connum != drawAeq[i](0)) {
			connum = drawAeq[i](0);
			inerdex = 0;
		}
		//cout << drawAineq[i](1) << endl;
		tpos(inerdex) = drawAeq[i](1);
		tcon(inerdex) = drawAeq[i](2);
		inerdex++;
		if (inerdex > 2) {
			glBegin(GL_LINES);
			glVertex3f(tpos(0), tpos(1), tpos(2));
			glVertex3f(tpos(0) + tcon(0), tpos(1) + tcon(1), tpos(2) + tcon(2));
			glEnd();
		}
	}
	//cout << "end" << endl;
}

#endif