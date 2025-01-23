#include "Preprocessor.h"

#include "remeshExtension.h"
#include "matlabOutputs.h"
#include "conversions.h"
#include "UtilEOL.h"

#include "external/ArcSim/geometry.hpp"
#include "external/ArcSim/subset.hpp"

#include <stdlib.h>

using namespace std;
using namespace Eigen;

double thresh = 0.025; // TODO:: Move
double boundary = 0.025; // TODO:: Move;

Vert *adjacent_vert(const Node *node, const Vert *vert);

double linepoint(const Vec3 &A, const Vec3 &B, const Vec3 &P)
{
	// Line: A -> B
	// Point: P
	Vec3 AP = P - A;
	Vec3 AB = B - A;
	double ab2 = dot(AB,AB);
	double apab = dot(AP,AB);
	return apab / ab2;
}

double linePointDist(const Vector3d &A, const Vector3d &B, const Vector3d &P) {
	// Line: A -> B
	// Point: P
	Vector3d AP = P - A;
	Vector3d AB = B - A;
	double ab2 = AB.dot(AB);
	double apab = AP.dot(AB);
	double t = apab / ab2;
	Vector3d newP  = (1.0 - t)*A + t*B;
	return (P - newP).norm();
}

void markWasEOL(Mesh& mesh) {
	for (int n = 0; n < mesh.nodes.size(); n++) {
		if (mesh.nodes[n]->EoL) {
			mesh.nodes[n]->EoL_state = Node::WasEOL;
		}
	}
}

bool inBoundaryQ(const MatrixXd &bounds, const Vector3d & P, const double &brange) {
	int b1, b2, corner = 0;
	for (b1 = 0; b1 < bounds.cols(); b1++) {
		if (b1 == bounds.cols() - 1) {
			b2 = 0;
		}
		else {
			b2 = b1 + 1;
		}
		if (linePointDist(bounds.block<3, 1>(0, b1), bounds.block<3, 1>(0, b2), P) < brange) return true;
	}
	return false;
}

bool inBoundaryN(const MatrixXd &bounds, const Vector3d & P, const Vec3 &e2, const double &brange) {
	int b1, b2, corner = 0;
	for (b1 = 0; b1 < bounds.cols(); b1++) {
		if (b1 == bounds.cols() - 1) {
			b2 = 0;
		}
		else {
			b2 = b1 + 1;
		}
		if (linePointDist(bounds.block<3, 1>(0, b1), bounds.block<3, 1>(0, b2), P) < brange) {
			corner++;
			if (corner > 1) return true; // This point is in a boundary corner and should not be EOL
			Vector3d A = bounds.block<3, 1>(0, b1);
			Vector3d B = bounds.block<3, 1>(0, b2);
			Vector3d AB = A - B;
			double angle = get_angle(e2v(AB), e2);
			if (angle < M_PI / 5 || angle >(4 * M_PI / 5)) return true;
		}
	}
	return false;
}

bool inBoundary(const MatrixXd &bounds, const Node* node, const double &brange) {
	// If this node is in a boundary, we want to check if its connected prserved edges are perpendicular within a range to the border edge
	if (!node->EoL) return false; // We are only checking for EoL points
	int b1, b2, corner = 0, preserved = 0;
	Vert * vert = node->verts[0];
	for (b1 = 0; b1 < bounds.cols(); b1++) {
		if (b1 == bounds.cols() - 1) {
			b2 = 0;
		}
		else {
			b2 = b1 + 1;
		}
		if (linePointDist(bounds.block<3, 1>(0, b1), bounds.block<3, 1>(0, b2), v2e(vert->u)) < brange) {
			if (node->cornerID >= 0) {
				return true; // This is an EoL corner and should 
			}
			corner++;
			if (corner > 1) {
				return true; // This point is in a boundary corner and should not be EOL
			}
			for (int e = 0; e < node->adje.size(); e++) {
				Edge *edge = node->adje[e];
				if (edge->preserve) {
					preserved++;
					Vector3d A = bounds.block<3, 1>(0, b1);
					Vector3d B = bounds.block<3, 1>(0, b2);
					Vector3d AB = A - B;
					Node *n1 = other_node(edge, node);
					double angle = get_angle(e2v(AB), (vert->u - n1->verts[0]->u));
					if (angle < M_PI / 5 || angle >(4 * M_PI / 5)) {
						return true;
					}
				}
			}
			// If we've it here then that means we have an EoL point that is in a boundary yet not a corner point and has no surrounding preserved edges
			// We'll make the assumption this is a lone EoL needing to be removed
			if (preserved == 0) {
				return true;
			}
		}
	}

	return false;
}

void addGeometry(Mesh& mesh, const MatrixXd &boundaries, const vector<shared_ptr<btc::Collision> > cls)
{
	for (int i = 0; i < cls.size(); i++) {
		// EOL nodes will be detected here
		// If they aren't found then the EOL node has been lifted off and we need to take note
		if (cls[i]->count1 == 3 && cls[i]->count2 == 1) {
			Node* node = mesh.nodes[cls[i]->verts2(0)];
			if (node->EoL) {
				node->EoL_state = Node::IsEOL;
			}
		}
		else if (cls[i]->count1 == 1 && cls[i]->count2 == 3) {
			if ((mesh.nodes[cls[i]->verts2(0)]->EoL && mesh.nodes[cls[i]->verts2(0)]->cornerID == cls[i]->verts1(0)) ||
				(mesh.nodes[cls[i]->verts2(1)]->EoL && mesh.nodes[cls[i]->verts2(1)]->cornerID == cls[i]->verts1(0)) ||
				(mesh.nodes[cls[i]->verts2(2)]->EoL && mesh.nodes[cls[i]->verts2(2)]->cornerID == cls[i]->verts1(0))) continue;
				
			// We don't add new points throughout this loop so this is safe
			Vert *v0 = mesh.verts[cls[i]->verts2(0)],
				*v1 = mesh.verts[cls[i]->verts2(1)],
				*v2 = mesh.verts[cls[i]->verts2(2)];
			//Face *f0 = mesh.faces[cls[i]->tri2];
				
			// TODO:: This may be overkill if the CD weights match the mesh barycoords
			double xX = cls[i]->weights2(0) * v0->u[0] +
				cls[i]->weights2(1) * v1->u[0] +
				cls[i]->weights2(2) * v2->u[0];
			double yX = cls[i]->weights2(0) * v0->u[1] +
				cls[i]->weights2(1) * v1->u[1] +
				cls[i]->weights2(2) * v2->u[1];

			// Boundary
			if (inBoundaryQ(boundaries, Vector3d(xX, yX, 0.0), boundary)) continue;
				
			// Faces CAN be added and deleted throughout this loop so we can't just use the CD returned tri
			Face *f0 = get_enclosing_face(mesh, Vec2(xX, yX));
			Vec3 bary = get_barycentric_coords(Vec2(xX, yX), f0);

			bool use_edge = false;
			for (int j = 0; j < 3; j++) {
				if (bary[j] < 1e-3) use_edge = true; // TODO:: No magic
			}

			// If this point is on a cloth edge, we should edge split instead
			if (use_edge) {
				double least = 1.0;
				int which = -1;
				for (int j = 0; j < 3; j++) {
					if (bary[j] < least) {
						least = bary[j];
						which = j;
					}
				}
				Edge *e0 = get_opp_edge(f0, f0->v[which]->node);
				double d;
				if (e0->n[0] == f0->v[0]->node) {
					d = 1.0 - bary[0];
				}
				else if (e0->n[0] == f0->v[1]->node) {
					d = 1.0 - bary[1];
				}
				else {
					d = 1.0 - bary[2];
				}
				Node *node0 = e0->n[0], *node1 = e0->n[1];
				RemeshOp op = split_edgeForced(e0, d, -1);
				for (size_t v = 0; v < op.added_verts.size(); v++) {
					Vert *vertnew = op.added_verts[v];
					Vert *v0 = adjacent_vert(node0, vertnew),
						*v1 = adjacent_vert(node1, vertnew);
					vertnew->sizing = 0.5 * (v0->sizing + v1->sizing);
				}
				op.done();
			}
			else {
				RemeshOp op = split_face(f0, bary);
				for (size_t v = 0; v < op.added_verts.size(); v++) {
					Vert *vertnew = op.added_verts[v];
					vertnew->sizing = (v0->sizing + v1->sizing + v2->sizing) / 3.0;
				}
				op.done();
			}

			Node *n = mesh.nodes.back();
			n->EoL = true;
			n->EoL_state = Node::NewEOL;
			n->preserve = true;
			n->cornerID = cls[i]->verts1(0);
			n->cdEdges = cls[i]->edge1;
			n->x = e2v(cls[i]->pos1_); // We want to offset the node slightly inside the obect so it gets detected by the CD until it moves out on it's own
		}
		if (cls[i]->count1 == 2 && cls[i]->count2 == 2) {
			// TODO:: Is this enough of a check?
			if (mesh.nodes[cls[i]->verts2(0)]->EoL ||
				mesh.nodes[cls[i]->verts2(1)]->EoL) continue;

			// The verts won't change since we only ever add verts
			Edge *e0 = get_edge(mesh.nodes[cls[i]->verts2(0)], mesh.nodes[cls[i]->verts2(1)]);
			double d;

			// We'll need this info for the boundary
			Vert *v0 = mesh.verts[cls[i]->verts2(0)],
				*v1 = mesh.verts[cls[i]->verts2(1)];

			double xX = cls[i]->weights2(0) * v0->u[0] +
				cls[i]->weights2(1) * v1->u[0];
			double yX = cls[i]->weights2(0) * v0->u[1] +
				cls[i]->weights2(1) * v1->u[1];

			Face *f0 = get_enclosing_face(mesh, Vec2(xX, yX));

			// Boundary simple
			MatrixXd F = deform_grad(f0);
			Vector2d e2 = F.transpose() * cls[i]->edgeDir;
			if (inBoundaryN(boundaries, Vector3d(xX, yX, 0.0), Vec3(e2(0), e2(1), 0.0), boundary)) continue;

			// If a previous collisions has already split this edge this get_edge will return NULL and we have to do a little more work to find it
			if (e0 == NULL) {

				// A barycentric tests largest two values should be the nodes of the new edge we need to split
				Vec3 bary = get_barycentric_coords(Vec2(xX, yX), f0);
				double least = 1.0;
				int which = -1;
				for (int j = 0; j < 3; j++) {
					if (bary[j] < least) {
						least = bary[j];
						which = j;
					}
				}
				e0 = get_opp_edge(f0, f0->v[which]->node);
				if (e0->n[0] == f0->v[0]->node) {
					d = 1.0 - bary[0];
				}
				else if (e0->n[0] == f0->v[1]->node) {
					d = 1.0 - bary[1];
				}
				else {
					d = 1.0 - bary[2];
				}
			}
			else {
				// ArcSim splits an edge using the weight from n[1]
				// Double check in case the CD does not index the same way
				if (e0->n[0]->index == cls[i]->verts2(1)) {
					d = cls[i]->weights2(0);
				}
				else {
					d = cls[i]->weights2(1);
				}
			}

			Node *node0 = e0->n[0], *node1 = e0->n[1];
			RemeshOp op = split_edgeForced(e0, d, -1);
			for (size_t v = 0; v < op.added_verts.size(); v++) {
				Vert *vertnew = op.added_verts[v];
				Vert *v0 = adjacent_vert(node0, vertnew),
					*v1 = adjacent_vert(node1, vertnew);
				vertnew->sizing = 0.5 * (v0->sizing + v1->sizing);
			}
			op.done();

			Node *n = mesh.nodes.back();
			n->EoL = true;
			n->EoL_state = Node::NewEOL;
			n->cdEdges = cls[i]->edge1;
			n->x = e2v(cls[i]->pos1_); // We want to offset the node slightly inside the obect so it gets detected by the CD until it moves out on it's own
		}
	}
}

void revertWasEOL(Mesh& mesh, const MatrixXd & bounds)
{
	// If something is still marked as WasEOL then it has lifted off
	for (int n = 0; n < mesh.nodes.size(); n++) {
		Node* node = mesh.nodes[n];
		if (node->EoL_state == Node::WasEOL) {
			node->EoL = false;
			node->preserve = false;
			node->cornerID = -1;
			node->cdEdges.clear();
		}
		// Boundary
		if (inBoundary(bounds, node, boundary)) {
			node->EoL_state == Node::WasEOL;
			node->EoL = false;
			node->preserve = false;
			node->cornerID = -1;
			node->cdEdges.clear();
		}
	}
}

void markPreserve(Mesh& mesh)
{
	for (int i = 0; i < mesh.edges.size(); i++) {
		Edge *e = mesh.edges[i];
		e->preserve = false;
		Node *n0 = e->n[0],
			*n1 = e->n[1];
		if (n0->EoL && n1->EoL) {
			// We never connect corners together
			// TODO:: Is this correct assumption?
			if (n0->cornerID >= 0 && n1->cornerID >= 0) continue;

			// We check the corner to edge, and edge to edge cases
			// A corner will have a list of cdEdges, while an edge is garaunteed to only have one cdEdge
			bool match = false;
			if (n0->cornerID >= 0) {
				for (int j = 0; j < n0->cdEdges.size(); j++) {
					if (n1->cdEdges[0] == n0->cdEdges[j]) {
						match = true;
						break;
					}
				}
			}
			else if (n1->cornerID >= 0) {
				for (int j = 0; j < n1->cdEdges.size(); j++) {
					if (n0->cdEdges[0] == n1->cdEdges[j]) {
						match = true;
						break;
					}
				}
			}
			else if (n0->cdEdges[0] == n1->cdEdges[0]) {
				match = true;
			}

			// If two EoL nodes share an edge AND they share a cdEdge ID
			// then the connected edge corresponds to object geometry and is preserved
			if (match) e->preserve = true;
		}
	}
}

double edge_metric(const Vert *vert0, const Vert *vert1);
bool can_collapseForced(const Edge *edge, int i) {
	for (int s = 0; s < 2; s++) {
		const Vert *vert0 = edge_vert(edge, s, i), *vert1 = edge_vert(edge, s, 1 - i);
		if (!vert0 || (s == 1 && vert0 == edge_vert(edge, 0, i)))
			continue;
		for (int f = 0; f < (int)vert0->adjf.size(); f++) {
			const Face *face = vert0->adjf[f];
			if (is_in(vert1, face->v))
				continue;
			const Vert *vs[3] = { face->v[0], face->v[1], face->v[2] };
			double a0 = norm(cross(vs[1]->u - vs[0]->u, vs[2]->u - vs[0]->u)) / 2;
			replace(vert0, vert1, vs);
			double a = norm(cross(vs[1]->u - vs[0]->u, vs[2]->u - vs[0]->u)) / 2;
			double asp = aspect(vs[0]->u, vs[1]->u, vs[2]->u);

			double sz = 0.1*sqr(0.5);

			if ((a < a0 && a < 1e-6) || asp < 1e-6) {
				bool get_later = false;
				for (int ne = 0; ne < 3; ne++) {
					int nep;
					ne == 2 ? nep = 0 : nep = ne + 1;
					if (unsigned_vv_distance(vs[ne]->u, vs[nep]->u) < thresh) {
						get_later = true;
						break;
					}
				}
				if (!get_later) return false;
			}
			//for (int e = 0; e < 3; e++)
			//	if (vs[e] != vert1 && edge_metric(vs[NEXT(e)], vs[PREV(e)]) > 0.9) {
			//		return false;
			//	}
		}
	}
	return true;
}

// For very illconditioned geometry we can potentially have two preserved edges forming a triangle
// We want to collapse this triangle and just make the non preserved edge the new single preserved edge
// This triangle was practically a line it was so thin that nothing is really being altered dramatically
void pass_collapse(RemeshOp op, Node *n)
{
	for (int e = 0; e < op.removed_edges.size(); e++) {
		if (op.removed_edges[e]->preserve) {
			if (op.removed_edges[e]->n[0] == n || op.removed_edges[e]->n[1] == n) continue;
			Edge *ep = get_edge(n, op.removed_edges[e]->n[0]);
			if(ep == NULL) ep = get_edge(n, op.removed_edges[e]->n[1]);
			if (ep != NULL) ep->preserve = true;
		}
	}
}

bool collapse_conformal(Mesh &mesh, bool &allclear)
{
	for (int i = 0; i < mesh.edges.size(); i++) {
		Edge *e = mesh.edges[i];
		if (e->preserve) {
			if (edge_length(e) < (2.0 * thresh)) {
				allclear = false;
				RemeshOp op;
				Node *n0 = e->n[0],
					*n1 = e->n[1];
				if (is_seam_or_boundary(n1) || n1->cornerID >= 0) {
					if (!can_collapseForced(e, 0)) continue;
					op = collapse_edgeForced(e, 0);
					if (op.empty()) continue;
					pass_collapse(op, n1);
					op.done();
					return true;
				}
				else if (is_seam_or_boundary(n0) || n0->cornerID >= 0) {
					if (!can_collapseForced(e, 1)) continue;
					op = collapse_edgeForced(e, 1);
					if (op.empty()) continue;
					pass_collapse(op, n0);
					op.done();
					return true;
				}
				else {
					if (n0->verts[0]->adjf.size() <= n1->verts[0]->adjf.size()) {
						if (!can_collapseForced(e, 1)) op = collapse_edgeForced(e, 1);
						if (op.empty()) {
							if (!can_collapseForced(e, 0)) continue;
							op = collapse_edgeForced(e, 0);
							if (op.empty()) {
								continue;
							}
							pass_collapse(op, n1);
						}
						else pass_collapse(op, n0);
					}
					else {
						if (!can_collapseForced(e, 0)) op = collapse_edgeForced(e, 0);
						if (op.empty()) {
							if (!can_collapseForced(e, 1)) continue;
							op = collapse_edgeForced(e, 1);
							if (op.empty()) {
								continue;
							}
							pass_collapse(op, n0);
						}
						else pass_collapse(op, n1);
					}
					op.done();
					return true;
				}
			}
		}
	}
	return false;
}

bool collapse_nonconformal(Mesh &mesh, bool &allclear)
{
	for (int i = 0; i < mesh.nodes.size(); i++) {
		Node *n = mesh.nodes[i];
		if (n->EoL) {
			Vert *v = n->verts[0];
			for (int f = 0; f < v->adjf.size(); f++) {
				for (int e = 0; e < 3; e++) {
					Edge *e0 = v->adjf[f]->adje[e];
					if (!e0->preserve && edge_length(e0) < thresh) {
						Node *n0 = e0->n[0],
							*n1 = e0->n[1];
						if (n0->EoL && n1->EoL) continue;
						//if (n0->preserve || n1->preserve) continue; // Don't mess with preserved points which are different from EoL points
						// Don't deal with edges between boundary and inside
						if (!(
							(is_seam_or_boundary(n0) && is_seam_or_boundary(n1)) ||
							(!is_seam_or_boundary(n0) && !is_seam_or_boundary(n1))
							)) continue;
						// These two loops should help fix some rare special cases, but may cause problems??
						bool worst = true;
						for (int ee = 0; ee < n0->adje.size(); ee++) {
							Edge *e1 = n0->adje[ee];
							if (edge_length(e1) < edge_length(e0)) worst = false;
						}
						for (int ee = 0; ee < n1->adje.size(); ee++) {
							Edge *e1 = n1->adje[ee];
							if (edge_length(e1) < edge_length(e0)) worst = false;
						}
						if (!worst) continue;
						allclear = false;
						RemeshOp op;
						if (n0->EoL) {
							if (!can_collapseForced(e0, 1)) continue;
							op = collapse_edgeForced(e0, 1);
							if (op.empty()) continue;
							op.done();
							return true;
						}
						else if (n1->EoL) {
							if (!can_collapseForced(e0, 0)) continue;
							op = collapse_edgeForced(e0, 0);
							if (op.empty()) continue;
							op.done();
							return true;
						}
						else {
							if (!n0->preserve) {
								if (can_collapseForced(e0, 0)) op = collapse_edgeForced(e0, 0);
								if (op.empty()) {
									if (!can_collapseForced(e0, 1)) continue;
									op = collapse_edgeForced(e0, 1);
									if (op.empty()) {
										continue;
									}
								}
							}
							else if (!n1->preserve) {
								if (can_collapseForced(e0, 1)) op = collapse_edgeForced(e0, 1);
								if (op.empty()) {
									if (!can_collapseForced(e0, 0)) continue;
									op = collapse_edgeForced(e0, 0);
									if (op.empty()) {
										continue;
									}
								}
							}
							op.done();
							return true;
						}
					}
				}
			}
		}
	}
	return false;
}

//bool collapse_close(Mesh &mesh)
//{
//	for (int i = 0; i < mesh.nodes.size(); i++) {
//		Node *n0 = mesh.nodes[i];
//		if (n0->EoL) {
//			for (int e = 0; e < n0->adje.size(); e++) {
//				Edge *e0 = n0->adje[e];
//				Node *n1 = other_node(e0, n0);
//				if (!n1->EoL && !is_seam_or_boundary(n1) && edge_length(e0) < thresh) {
//					RemeshOp op;
//					if (n0 == e0->n[0]) op = collapse_edgeForced(e0, 1);
//					else op = collapse_edgeForced(e0, 0);
//					if (op.empty()) continue;
//					op.done();
//					return true;
//				}
//			}
//		}
//	}
//	return false;
//}

int conformalCount(Face *f)
{
	int count = 0;
	for (int vert = 0; vert < 3; vert++) {
		if (f->v[vert]->node->EoL) count++;
	}
	return count;
}

Node *single_eol_from_face(Face *f)
{
	for (int vert = 0; vert < 3; vert++) {
		if (f->v[vert]->node->EoL) return f->v[vert]->node;
	}
	return NULL;
}

Edge *single_conformal_edge_from_face(Face *f)
{
	for (int e = 0; e < 3; e++) {
		if (f->adje[e]->preserve) return f->adje[e];
	}
	return NULL;
}

Edge *single_nonconformal_edge_from_face(Face *f)
{
	for (int e = 0; e < 3; e++) {
		if (!f->adje[e]->preserve) return f->adje[e];
	}
	return NULL;
}

double face_altitude(Edge* edge, Face* face) {
	return (2 * area(face)) / edge_length(edge);
}


// TODO:: Better metric than face altitude?
bool split_illconditioned_faces(Mesh &mesh)
{
	vector<Edge*> bad_edges;
	vector<int> case_pair;
	vector<double> split_point;
	for (int i = 0; i < mesh.faces.size(); i++) {
		Face *f0 = mesh.faces[i];
		int cc = conformalCount(f0);
		if (cc == 1) {
			Node *n0 = single_eol_from_face(f0);
			Edge *e0 = get_opp_edge(f0, n0);
			double faceAlti = face_altitude(e0, f0);
			if (faceAlti < thresh / 2) {
				double sp = 0.5;
				// Special case where it's better if we split along a different segment
				for (int ff = 0; ff < n0->verts[0]->adjf.size(); ff++) {
					Face *f1 = n0->verts[0]->adjf[ff];
					if (f0 == f1) continue;
					Edge *e1 = get_opp_edge(f1, n0);
					if (face_altitude(e1, f1) < faceAlti) {
						e0 = e1;
						sp = (linepoint(e0->n[0]->x, e0->n[1]->x, n0->x));
					}
				}
				if (sp < 0.0 || sp > 1.0) continue; // Just to avoid problems, although if it happens there might already be a problem
				bad_edges.push_back(e0);
				case_pair.push_back(1);
				split_point.push_back(sp);
			}
		}
		// TODO:: Cases 2 and 3 may break if large triangles share individual EoL points without preserved edges
		else if (cc == 2) {
			Edge *e0 = single_conformal_edge_from_face(f0);
			if (e0 == NULL) continue; // If two corner EoL points share a triangle with no preserved edge
			if (face_altitude(e0, f0) < (thresh / 2)) {
				bad_edges.push_back(e0);
				case_pair.push_back(2);
				split_point.push_back(0.5);
			}
		}
		else if(cc == 3) {
			Edge *e0 = single_nonconformal_edge_from_face(f0);
			if (face_altitude(e0, f0) < thresh / 2) {
				bad_edges.push_back(e0);
				case_pair.push_back(3);
				split_point.push_back(0.5);
			}
		}
	}
	int exclude = 0;
	for (size_t e = 0; e < bad_edges.size(); e++) {
		Edge *edge = bad_edges[e];
		if (!edge) continue;
		Node *node0 = edge->n[0], *node1 = edge->n[1];
		RemeshOp op = split_edgeForced(edge, split_point[e], thresh);
		if (op.empty()) {
			exclude++;
			continue;
		}
		for (size_t v = 0; v < op.added_verts.size(); v++) {
			Vert *vertnew = op.added_verts[v];
			Vert *v0 = adjacent_vert(node0, vertnew),
				*v1 = adjacent_vert(node1, vertnew);
			vertnew->sizing = 0.5 * (v0->sizing + v1->sizing);
		}

		// If we've split a conformal edge, this new node is EoL and must have the appropriate data
		if (case_pair[e] == 2) {
			Node *node = op.added_nodes[0]; // There should only be one?
			node->EoL = true;
			if (node0->EoL_state == Node::IsEOL && node1->EoL_state == Node::IsEOL) node->EoL_state = Node::NewEOLFromSplit;
			else node->EoL_state = Node::NewEOL;
			// Transfer cdEdges from the non corner EoL node
			// This should be safe?
			if (node0->cornerID >= 0) node->cdEdges = node1->cdEdges;
			else node->cdEdges = node0->cdEdges;
		}
		op.set_null(bad_edges);
		op.done();
	}
	return bad_edges.size() == 0 + exclude;
}

void flip_edges(MeshSubset* subset, vector<Face*>& active_faces,
	vector<Edge*>* update_edges, vector<Face*>* update_faces);


// TODO:: Does conformal stalling occur with EOL?
void cleanup(Mesh& mesh)
{
	vector<Face*> active_faces = mesh.faces;
	flip_edges(0, active_faces, 0, 0);
	markPreserve(mesh);
	bool allclear = false;
	while (!allclear) {
		// Iterate until all the bad edges are accounted for
		// If a bad edge exists, but is unsafe to collapse, try in the next iteration where it may become safe, or another operation may take care of it
		while(!allclear) {
			allclear = true;
			while (collapse_nonconformal(mesh, allclear));
			markPreserve(mesh); // Can be removed if the conformal pass doesn't loop over edges and check for preserve status
			while (collapse_conformal(mesh, allclear));
		}
		allclear = split_illconditioned_faces(mesh);
		//allclear = true;
	}
	markPreserve(mesh); // Probably doesn't need to be called so much, but wan't to be safe
}

// TODO:: I think there are problems when a box corner reaches the cloth border and the two box edges still move through the cloth

void preprocess(Mesh& mesh, const MatrixXd &boundaries, const vector<shared_ptr<btc::Collision> > cls)
{
	markWasEOL(mesh);
	addGeometry(mesh, boundaries, cls);
	//revertWasEOL(mesh, boundaries);

	//markPreserve(mesh);

	cleanup(mesh);

	revertWasEOL(mesh, boundaries);

	compute_ws_data(mesh);
}

void preprocessClean(Mesh& mesh)
{
	cleanup(mesh);
	compute_ws_data(mesh);
}

bool a;
void preprocessPart(Mesh& mesh, const MatrixXd &boundaries, const vector<shared_ptr<btc::Collision> > cls, int &part)
{
	if (part == 1) {
		//for (int i = 0; i < mesh.nodes.size(); i++) {
		//	mesh.nodes[i]->EoL = false;
		//	mesh.nodes[i]->cornerID = -1;
		//	mesh.nodes[i]->cdEdges.clear();
		//}
		//for (int i = 0; i < mesh.edges.size(); i++) {
		//	mesh.edges[i]->preserve = false;
		//}
		a = true;
		markWasEOL(mesh);
		addGeometry(mesh, boundaries, cls);
		revertWasEOL(mesh, boundaries);
		cout << "Add Geometry" << endl;
	}
	else if (part == 2) {
		markPreserve(mesh);
		cout << "Mark preserve" << endl;
		//part = 6;
	}
	else if (part == 3) {
		vector<Face*> active_faces = mesh.faces;
		flip_edges(0, active_faces, 0, 0);
		markPreserve(mesh);
		cout << "Flipped edges" << endl;
	}
	else if (part == 4) {
		a = true;
		while (collapse_nonconformal(mesh,a));
		markPreserve(mesh);
		cout << "Collapse nonfornformal" << endl;
	}
	else if (part == 5) {
		while (collapse_conformal(mesh,a));
		if (!a) part = 3;
		cout << "Collapse conformal" << endl;
	}
	else if (part == 6) {
		bool allclear = split_illconditioned_faces(mesh);
		if (!allclear) {
			part = 3;
			cout << "Split ill-conditioned, not good" << endl;
		}
		else {
			markPreserve(mesh);
			cout << "Split ill-conditioned, all good" << endl;
		}
	}
	else if (part == 7) {
		compute_ws_data(mesh);
		cout << "Compute ws data" << endl;
	}
}