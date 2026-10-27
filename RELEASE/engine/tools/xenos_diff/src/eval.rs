//! A CPU executor for naga IR, wide enough for the WGSL `xenos_ucode::wgsl` writes:
//! scalars, vectors, arrays and structs of f32 / i32 / u32 / bool, the two storage
//! buffers (`arena`, `guest`), function calls, `loop` / `switch` / `if`.
//!
//! What it does where WGSL leaves a choice, so the checker can compare with the SDK's
//! interpreter (which computes in plain C++ floats):
//! - IEEE f32 arithmetic, never fused; `dot` adds left to right; `inverseSqrt(x)` is
//!   `1 / sqrt(x)`; `exp2`, `log2`, `sin`, `cos` are the C library's (as `std::` in C++).
//! - `discard` marks the invocation and goes on (WGSL's demote-to-helper), so the
//!   values stay comparable (the interpreter doesn't kill pixels).
//! - Texture samples and loads read (0, 0, 0, 0) and `textureDimensions` 1, as the
//!   interpreter writes zeros for every texture fetch; derivatives are 0.
//! - Storage reads outside a buffer read 0.

use std::cell::{Cell, RefCell};

use naga::{
    Arena, BinaryOperator, Binding, Block, Expression, Function, Handle, ImageDimension, Literal,
    MathFunction, Module, RelationalFunction, ScalarKind, Statement, SwitchValue, TypeInner, UnaryOperator,
};

#[derive(Clone, Copy, Debug, PartialEq)]
pub enum S {
    F(f32),
    I(i32),
    U(u32),
    B(bool),
}

impl S {
    pub fn f(self) -> f32 {
        match self {
            S::F(x) => x,
            _ => panic!("not an f32: {:?}", self),
        }
    }
    fn u(self) -> u32 {
        match self {
            S::U(x) => x,
            S::I(x) => x as u32,
            _ => panic!("not an integer: {:?}", self),
        }
    }
    fn b(self) -> bool {
        match self {
            S::B(x) => x,
            _ => panic!("not a bool: {:?}", self),
        }
    }
}

#[derive(Clone, Copy, Debug)]
pub enum Root {
    Local(u32),
    Global(u32),
}

#[derive(Clone, Copy, Debug)]
pub struct Ptr {
    root: Root,
    path: [u32; 4],
    len: u8,
}

#[derive(Clone, Debug)]
pub enum V {
    S(S),
    Vec(u8, [S; 4]),
    Agg(Vec<V>),
    Ptr(Ptr),
    Opaque,
}

impl V {
    pub fn comps(&self) -> Vec<S> {
        match self {
            V::S(s) => vec![*s],
            V::Vec(n, c) => c[..*n as usize].to_vec(),
            _ => panic!("not a scalar or vector: {:?}", self),
        }
    }
    fn s(&self) -> S {
        match self {
            V::S(s) => *s,
            _ => panic!("not a scalar: {:?}", self),
        }
    }
}

/// The two storage buffers, as raw words.
pub struct Buffers {
    pub arena: Vec<[u32; 4]>,
    pub guest: Vec<u32>,
}

#[derive(Clone, Copy, PartialEq)]
enum GlobalKind {
    Arena,
    Guest,
    Opaque,
}

enum Flow {
    Normal,
    Break,
    Continue,
    Return(Option<V>),
}

struct Frame<'a> {
    exprs: &'a Arena<Expression>,
    vals: Vec<Option<V>>,
    locals: Vec<V>,
    args: Vec<V>,
}

pub struct Exec<'m> {
    m: &'m Module,
    globals: Vec<GlobalKind>,
    consts: RefCell<Vec<Option<V>>>,
    pub killed: Cell<bool>,
    steps: Cell<u64>,
    pub step_limit: u64,
}

/// What the entry point returned, and its local variables when it returned.
pub struct Outcome {
    pub result: Option<V>,
    pub locals: Vec<(Option<String>, V)>,
}

type R<T> = Result<T, String>;

fn map1(v: &V, f: impl Fn(S) -> S) -> V {
    match v {
        V::S(s) => V::S(f(*s)),
        V::Vec(n, c) => {
            let mut o = *c;
            for x in o.iter_mut().take(*n as usize) {
                *x = f(*x);
            }
            V::Vec(*n, o)
        }
        _ => panic!("elementwise on {:?}", v),
    }
}

fn map2(a: &V, b: &V, f: impl Fn(S, S) -> S) -> V {
    match (a, b) {
        (V::S(x), V::S(y)) => V::S(f(*x, *y)),
        (V::Vec(n, x), V::Vec(m, y)) => {
            assert_eq!(n, m, "vector sizes");
            let mut o = *x;
            for i in 0..*n as usize {
                o[i] = f(x[i], y[i]);
            }
            V::Vec(*n, o)
        }
        (V::Vec(n, x), V::S(y)) => {
            let mut o = *x;
            for i in 0..*n as usize {
                o[i] = f(x[i], *y);
            }
            V::Vec(*n, o)
        }
        (V::S(x), V::Vec(n, y)) => {
            let mut o = *y;
            for i in 0..*n as usize {
                o[i] = f(*x, y[i]);
            }
            V::Vec(*n, o)
        }
        _ => panic!("elementwise on {:?} and {:?}", a, b),
    }
}

fn map3(a: &V, b: &V, c: &V, f: impl Fn(S, S, S) -> S) -> V {
    let n = [a, b, c]
        .iter()
        .map(|v| match v {
            V::Vec(n, _) => *n,
            _ => 1,
        })
        .max()
        .unwrap();
    let get = |v: &V, i: usize| match v {
        V::S(s) => *s,
        V::Vec(_, c) => c[i],
        _ => panic!("elementwise on {:?}", v),
    };
    if n == 1 && matches!(a, V::S(_)) {
        return V::S(f(get(a, 0), get(b, 0), get(c, 0)));
    }
    let mut o = [S::F(0.0); 4];
    for (i, x) in o.iter_mut().enumerate().take(n as usize) {
        *x = f(get(a, i), get(b, i), get(c, i));
    }
    V::Vec(n, o)
}

fn half_to_f32(h: u32) -> f32 {
    let h = h & 0xFFFF;
    let sign = (h >> 15) & 1;
    let exp = (h >> 10) & 0x1F;
    let man = h & 0x3FF;
    let v = if exp == 0 {
        (man as f32) * (2.0f32).powi(-24)
    } else if exp == 31 {
        if man == 0 {
            f32::INFINITY
        } else {
            f32::from_bits(0x7FC0_0000 | (man << 13))
        }
    } else {
        f32::from_bits(((exp + 112) << 23) | (man << 13))
    };
    if sign == 1 {
        -v
    } else {
        v
    }
}

fn extract_bits(e: S, offset: u32, count: u32) -> S {
    let o = offset.min(32);
    let c = count.min(32 - o);
    match e {
        S::U(x) => {
            if c == 0 {
                S::U(0)
            } else if c == 32 {
                S::U(x)
            } else {
                S::U((x >> o) & ((1u32 << c) - 1))
            }
        }
        S::I(x) => {
            if c == 0 {
                S::I(0)
            } else if c == 32 {
                S::I(x)
            } else {
                S::I(((x as u32) << (32 - o - c)) as i32 >> (32 - c))
            }
        }
        _ => panic!("extractBits on {:?}", e),
    }
}

fn bin_s(op: BinaryOperator, x: S, y: S) -> S {
    use BinaryOperator as B;
    match (x, y) {
        (S::F(a), S::F(b)) => match op {
            B::Add => S::F(a + b),
            B::Subtract => S::F(a - b),
            B::Multiply => S::F(a * b),
            B::Divide => S::F(a / b),
            B::Modulo => S::F(a % b),
            B::Equal => S::B(a == b),
            B::NotEqual => S::B(a != b),
            B::Less => S::B(a < b),
            B::LessEqual => S::B(a <= b),
            B::Greater => S::B(a > b),
            B::GreaterEqual => S::B(a >= b),
            _ => panic!("{:?} on floats", op),
        },
        (S::I(a), S::I(b)) => match op {
            B::Add => S::I(a.wrapping_add(b)),
            B::Subtract => S::I(a.wrapping_sub(b)),
            B::Multiply => S::I(a.wrapping_mul(b)),
            B::Divide => S::I(if b == 0 || (a == i32::MIN && b == -1) { a } else { a / b }),
            B::Modulo => S::I(if b == 0 || (a == i32::MIN && b == -1) { 0 } else { a % b }),
            B::Equal => S::B(a == b),
            B::NotEqual => S::B(a != b),
            B::Less => S::B(a < b),
            B::LessEqual => S::B(a <= b),
            B::Greater => S::B(a > b),
            B::GreaterEqual => S::B(a >= b),
            B::And => S::I(a & b),
            B::InclusiveOr => S::I(a | b),
            B::ExclusiveOr => S::I(a ^ b),
            _ => panic!("{:?} on i32", op),
        },
        (S::I(a), S::U(b)) => match op {
            B::ShiftLeft => S::I(((a as u32) << (b & 31)) as i32),
            B::ShiftRight => S::I(a >> (b & 31)),
            _ => panic!("{:?} on i32, u32", op),
        },
        (S::U(a), S::U(b)) => match op {
            B::Add => S::U(a.wrapping_add(b)),
            B::Subtract => S::U(a.wrapping_sub(b)),
            B::Multiply => S::U(a.wrapping_mul(b)),
            B::Divide => S::U(if b == 0 { a } else { a / b }),
            B::Modulo => S::U(if b == 0 { 0 } else { a % b }),
            B::Equal => S::B(a == b),
            B::NotEqual => S::B(a != b),
            B::Less => S::B(a < b),
            B::LessEqual => S::B(a <= b),
            B::Greater => S::B(a > b),
            B::GreaterEqual => S::B(a >= b),
            B::And => S::U(a & b),
            B::InclusiveOr => S::U(a | b),
            B::ExclusiveOr => S::U(a ^ b),
            B::ShiftLeft => S::U(a << (b & 31)),
            B::ShiftRight => S::U(a >> (b & 31)),
            _ => panic!("{:?} on u32", op),
        },
        (S::B(a), S::B(b)) => match op {
            B::And | B::LogicalAnd => S::B(a && b),
            B::InclusiveOr | B::LogicalOr => S::B(a || b),
            B::Equal => S::B(a == b),
            B::NotEqual | B::ExclusiveOr => S::B(a != b),
            _ => panic!("{:?} on bools", op),
        },
        _ => panic!("{:?} on {:?} and {:?}", op, x, y),
    }
}

fn convert(x: S, kind: ScalarKind) -> S {
    match (kind, x) {
        (ScalarKind::Float, S::F(a)) => S::F(a),
        (ScalarKind::Float, S::I(a)) => S::F(a as f32),
        (ScalarKind::Float, S::U(a)) => S::F(a as f32),
        (ScalarKind::Float, S::B(a)) => S::F(if a { 1.0 } else { 0.0 }),
        (ScalarKind::Sint, S::F(a)) => S::I(a as i32),
        (ScalarKind::Sint, S::I(a)) => S::I(a),
        (ScalarKind::Sint, S::U(a)) => S::I(a as i32),
        (ScalarKind::Sint, S::B(a)) => S::I(a as i32),
        (ScalarKind::Uint, S::F(a)) => S::U(a as u32),
        (ScalarKind::Uint, S::I(a)) => S::U(a as u32),
        (ScalarKind::Uint, S::U(a)) => S::U(a),
        (ScalarKind::Uint, S::B(a)) => S::U(a as u32),
        (ScalarKind::Bool, S::F(a)) => S::B(a != 0.0),
        (ScalarKind::Bool, S::I(a)) => S::B(a != 0),
        (ScalarKind::Bool, S::U(a)) => S::B(a != 0),
        (ScalarKind::Bool, S::B(a)) => S::B(a),
        _ => panic!("conversion of {:?} to {:?}", x, kind),
    }
}

fn bitcast(x: S, kind: ScalarKind) -> S {
    let bits = match x {
        S::F(a) => a.to_bits(),
        S::I(a) => a as u32,
        S::U(a) => a,
        S::B(_) => panic!("bitcast of a bool"),
    };
    match kind {
        ScalarKind::Float => S::F(f32::from_bits(bits)),
        ScalarKind::Sint => S::I(bits as i32),
        ScalarKind::Uint => S::U(bits),
        _ => panic!("bitcast to {:?}", kind),
    }
}

fn fmin(a: f32, b: f32) -> f32 {
    a.min(b)
}
fn fmax(a: f32, b: f32) -> f32 {
    a.max(b)
}

fn math1(fun: MathFunction, x: S) -> S {
    use MathFunction as M;
    match x {
        S::F(a) => S::F(match fun {
            M::Abs => a.abs(),
            M::Floor => a.floor(),
            M::Ceil => a.ceil(),
            M::Trunc => a.trunc(),
            M::Round => a.round_ties_even(),
            M::Fract => a - a.floor(),
            M::Sqrt => a.sqrt(),
            M::InverseSqrt => 1.0 / a.sqrt(),
            M::Exp2 => a.exp2(),
            M::Log2 => a.log2(),
            M::Exp => a.exp(),
            M::Log => a.ln(),
            M::Sin => a.sin(),
            M::Cos => a.cos(),
            M::Saturate => fmin(fmax(a, 0.0), 1.0),
            M::Sign => {
                if a > 0.0 {
                    1.0
                } else if a < 0.0 {
                    -1.0
                } else {
                    0.0
                }
            }
            _ => panic!("{:?} on a float", fun),
        }),
        S::I(a) => S::I(match fun {
            M::Abs => a.wrapping_abs(),
            M::Sign => a.signum(),
            _ => panic!("{:?} on i32", fun),
        }),
        S::U(a) => S::U(match fun {
            M::Abs => a,
            _ => panic!("{:?} on u32", fun),
        }),
        S::B(_) => panic!("{:?} on a bool", fun),
    }
}

fn math2(fun: MathFunction, x: S, y: S) -> S {
    use MathFunction as M;
    match (x, y) {
        (S::F(a), S::F(b)) => S::F(match fun {
            M::Min => fmin(a, b),
            M::Max => fmax(a, b),
            M::Pow => a.powf(b),
            M::Step => {
                if b >= a {
                    1.0
                } else {
                    0.0
                }
            }
            _ => panic!("{:?} on floats", fun),
        }),
        (S::I(a), S::I(b)) => S::I(match fun {
            M::Min => a.min(b),
            M::Max => a.max(b),
            _ => panic!("{:?} on i32", fun),
        }),
        (S::U(a), S::U(b)) => S::U(match fun {
            M::Min => a.min(b),
            M::Max => a.max(b),
            _ => panic!("{:?} on u32", fun),
        }),
        _ => panic!("{:?} on {:?}, {:?}", fun, x, y),
    }
}

impl<'m> Exec<'m> {
    pub fn new(m: &'m Module) -> Self {
        let globals = m
            .global_variables
            .iter()
            .map(|(_, g)| match g.name.as_deref() {
                Some("arena") => GlobalKind::Arena,
                Some("guest") => GlobalKind::Guest,
                _ => GlobalKind::Opaque,
            })
            .collect();
        Exec {
            m,
            globals,
            consts: RefCell::new(vec![None; m.constants.len()]),
            killed: Cell::new(false),
            steps: Cell::new(0),
            step_limit: 20_000_000,
        }
    }

    fn zero(&self, ty: Handle<naga::Type>) -> V {
        match &self.m.types[ty].inner {
            TypeInner::Scalar(s) => V::S(zero_s(s.kind)),
            TypeInner::Vector { size, scalar } => V::Vec(*size as u8, [zero_s(scalar.kind); 4]),
            TypeInner::Array { base, size: naga::ArraySize::Constant(n), .. } => {
                V::Agg(vec![self.zero(*base); n.get() as usize])
            }
            TypeInner::Struct { members, .. } => V::Agg(members.iter().map(|m| self.zero(m.ty)).collect()),
            TypeInner::Atomic(s) => V::S(zero_s(s.kind)),
            _ => V::Opaque,
        }
    }

    fn constant(&self, c: Handle<naga::Constant>) -> V {
        if let Some(v) = &self.consts.borrow()[c.index()] {
            return v.clone();
        }
        let init = self.m.constants[c].init;
        let mut f = Frame {
            exprs: &self.m.global_expressions,
            vals: vec![None; self.m.global_expressions.len()],
            locals: Vec::new(),
            args: Vec::new(),
        };
        let v = self.eval(&mut f, init).expect("a constant");
        self.consts.borrow_mut()[c.index()] = Some(v.clone());
        v
    }

    fn eval(&self, f: &mut Frame, h: Handle<Expression>) -> R<V> {
        if let Some(v) = &f.vals[h.index()] {
            return Ok(v.clone());
        }
        let v = self.compute(f, h)?;
        f.vals[h.index()] = Some(v.clone());
        Ok(v)
    }

    fn image_dim(&self, f: &Frame, h: Handle<Expression>) -> Option<ImageDimension> {
        match &f.exprs[h] {
            Expression::Access { base, .. } | Expression::AccessIndex { base, .. } => self.image_dim(f, *base),
            Expression::GlobalVariable(g) => {
                let mut ty = self.m.global_variables[*g].ty;
                loop {
                    match &self.m.types[ty].inner {
                        TypeInner::BindingArray { base, .. } => ty = *base,
                        TypeInner::Image { dim, .. } => return Some(*dim),
                        _ => return None,
                    }
                }
            }
            _ => None,
        }
    }

    fn compute(&self, f: &mut Frame, h: Handle<Expression>) -> R<V> {
        let exprs = f.exprs;
        let e = &exprs[h];
        Ok(match e {
            Expression::Literal(l) => V::S(match *l {
                Literal::F32(x) => S::F(x),
                Literal::U32(x) => S::U(x),
                Literal::I32(x) => S::I(x),
                Literal::Bool(x) => S::B(x),
                Literal::AbstractInt(x) => S::I(x as i32),
                Literal::AbstractFloat(x) => S::F(x as f32),
                Literal::F64(x) => S::F(x as f32),
                ref other => return Err(format!("literal {:?}", other)),
            }),
            Expression::Constant(c) => self.constant(*c),
            Expression::ZeroValue(ty) => self.zero(*ty),
            Expression::Compose { ty, components } => {
                let comps: Vec<V> = components.iter().map(|c| self.eval(f, *c)).collect::<R<_>>()?;
                match &self.m.types[*ty].inner {
                    TypeInner::Vector { size, .. } => {
                        let mut o = [S::F(0.0); 4];
                        let mut i = 0;
                        for c in &comps {
                            for s in c.comps() {
                                o[i] = s;
                                i += 1;
                            }
                        }
                        assert_eq!(i, *size as usize, "compose size");
                        V::Vec(*size as u8, o)
                    }
                    _ => V::Agg(comps),
                }
            }
            Expression::Access { base, index } => {
                let b = self.eval(f, *base)?;
                let i = self.eval(f, *index)?.s().u();
                access(b, i)
            }
            Expression::AccessIndex { base, index } => {
                let b = self.eval(f, *base)?;
                access(b, *index)
            }
            Expression::Splat { size, value } => {
                let s = self.eval(f, *value)?.s();
                V::Vec(*size as u8, [s; 4])
            }
            Expression::Swizzle { size, vector, pattern } => {
                let v = self.eval(f, *vector)?;
                let c = v.comps();
                let mut o = [S::F(0.0); 4];
                for i in 0..*size as usize {
                    o[i] = c[pattern[i] as usize];
                }
                V::Vec(*size as u8, o)
            }
            Expression::FunctionArgument(i) => f.args[*i as usize].clone(),
            Expression::GlobalVariable(g) => match self.globals[g.index()] {
                GlobalKind::Opaque => V::Opaque,
                _ => V::Ptr(Ptr { root: Root::Global(g.index() as u32), path: [0; 4], len: 0 }),
            },
            Expression::LocalVariable(l) => {
                V::Ptr(Ptr { root: Root::Local(l.index() as u32), path: [0; 4], len: 0 })
            }
            Expression::Load { pointer } => {
                let p = self.eval(f, *pointer)?;
                match p {
                    V::Ptr(p) => self.load(f, p),
                    V::Opaque => V::Opaque,
                    other => return Err(format!("load of {:?}", other)),
                }
            }
            Expression::ImageSample { .. } | Expression::ImageLoad { .. } => V::Vec(4, [S::F(0.0); 4]),
            Expression::ImageQuery { image, .. } => {
                let n = match self.image_dim(f, *image) {
                    Some(ImageDimension::D1) => 1,
                    Some(ImageDimension::D3) => 3,
                    _ => 2,
                };
                if n == 1 {
                    V::S(S::U(1))
                } else {
                    V::Vec(n, [S::U(1); 4])
                }
            }
            Expression::Unary { op, expr } => {
                let v = self.eval(f, *expr)?;
                let op = *op;
                map1(&v, |x| match (op, x) {
                    (UnaryOperator::Negate, S::F(a)) => S::F(-a),
                    (UnaryOperator::Negate, S::I(a)) => S::I(a.wrapping_neg()),
                    (UnaryOperator::LogicalNot, S::B(a)) | (UnaryOperator::BitwiseNot, S::B(a)) => S::B(!a),
                    (UnaryOperator::BitwiseNot, S::U(a)) => S::U(!a),
                    (UnaryOperator::BitwiseNot, S::I(a)) => S::I(!a),
                    _ => panic!("{:?} on {:?}", op, x),
                })
            }
            Expression::Binary { op, left, right } => {
                let a = self.eval(f, *left)?;
                let b = self.eval(f, *right)?;
                let op = *op;
                map2(&a, &b, |x, y| bin_s(op, x, y))
            }
            Expression::Select { condition, accept, reject } => {
                let c = self.eval(f, *condition)?;
                let a = self.eval(f, *accept)?;
                let r = self.eval(f, *reject)?;
                match c {
                    V::S(S::B(b)) => {
                        if b {
                            a
                        } else {
                            r
                        }
                    }
                    V::Vec(n, cs) => {
                        let (ac, rc) = (a.comps(), r.comps());
                        let mut o = [S::F(0.0); 4];
                        for i in 0..n as usize {
                            o[i] = if cs[i].b() { ac[i] } else { rc[i] };
                        }
                        V::Vec(n, o)
                    }
                    other => return Err(format!("select on {:?}", other)),
                }
            }
            Expression::Derivative { expr, .. } => {
                let v = self.eval(f, *expr)?;
                map1(&v, |_| S::F(0.0))
            }
            Expression::Relational { fun, argument } => {
                let v = self.eval(f, *argument)?;
                match fun {
                    RelationalFunction::All => V::S(S::B(v.comps().iter().all(|s| s.b()))),
                    RelationalFunction::Any => V::S(S::B(v.comps().iter().any(|s| s.b()))),
                    RelationalFunction::IsNan => map1(&v, |s| S::B(s.f().is_nan())),
                    RelationalFunction::IsInf => map1(&v, |s| S::B(s.f().is_infinite())),
                }
            }
            Expression::Math { fun, arg, arg1, arg2, .. } => {
                let fun = *fun;
                let a = self.eval(f, *arg)?;
                let b = match arg1 {
                    Some(x) => Some(self.eval(f, *x)?),
                    None => None,
                };
                let c = match arg2 {
                    Some(x) => Some(self.eval(f, *x)?),
                    None => None,
                };
                match fun {
                    MathFunction::Clamp => {
                        let (b, c) = (b.unwrap(), c.unwrap());
                        map3(&a, &b, &c, |x, lo, hi| math2(MathFunction::Min, math2(MathFunction::Max, x, lo), hi))
                    }
                    MathFunction::ExtractBits => {
                        let (o, n) = (b.unwrap().s().u(), c.unwrap().s().u());
                        map1(&a, |x| extract_bits(x, o, n))
                    }
                    MathFunction::Dot => {
                        let (x, y) = (a.comps(), b.unwrap().comps());
                        let mut acc = x[0].f() * y[0].f();
                        for i in 1..x.len() {
                            acc += x[i].f() * y[i].f();
                        }
                        V::S(S::F(acc))
                    }
                    MathFunction::Unpack2x16float => {
                        let u = a.s().u();
                        V::Vec(2, [S::F(half_to_f32(u)), S::F(half_to_f32(u >> 16)), S::F(0.0), S::F(0.0)])
                    }
                    MathFunction::Min | MathFunction::Max | MathFunction::Pow | MathFunction::Step => {
                        map2(&a, &b.unwrap(), |x, y| math2(fun, x, y))
                    }
                    _ => map1(&a, |x| math1(fun, x)),
                }
            }
            Expression::As { expr, kind, convert: conv } => {
                let v = self.eval(f, *expr)?;
                let kind = *kind;
                if conv.is_some() {
                    map1(&v, |x| convert(x, kind))
                } else {
                    map1(&v, |x| bitcast(x, kind))
                }
            }
            Expression::ArrayLength(e) => {
                let p = self.eval(f, *e)?;
                match p {
                    V::Ptr(Ptr { root: Root::Global(g), .. }) => match self.globals[g as usize] {
                        GlobalKind::Arena => V::S(S::U(0)),
                        _ => V::S(S::U(0)),
                    },
                    _ => V::S(S::U(0)),
                }
            }
            Expression::CallResult(_) => return Err("a call result read before its call".into()),
            other => return Err(format!("unsupported expression {:?}", other)),
        })
    }

    fn load(&self, f: &Frame, p: Ptr) -> V {
        match p.root {
            Root::Local(l) => load_path(&f.locals[l as usize], &p.path[..p.len as usize]),
            Root::Global(g) => {
                let b = BUFFERS.with(|b| b.get());
                // SAFETY: set by `run` for the duration of the call.
                let b = unsafe { &*b };
                match self.globals[g as usize] {
                    GlobalKind::Arena => {
                        let row = b.arena.get(p.path[0] as usize).copied().unwrap_or([0; 4]);
                        if p.len >= 2 {
                            V::S(S::F(f32::from_bits(row[(p.path[1] as usize).min(3)])))
                        } else {
                            V::Vec(4, row.map(|w| S::F(f32::from_bits(w))))
                        }
                    }
                    GlobalKind::Guest => V::S(S::U(b.guest.get(p.path[0] as usize).copied().unwrap_or(0))),
                    GlobalKind::Opaque => V::Opaque,
                }
            }
        }
    }

    fn step(&self) -> R<()> {
        let n = self.steps.get() + 1;
        self.steps.set(n);
        if n > self.step_limit {
            return Err("ran too long (a runaway loop?)".into());
        }
        Ok(())
    }

    fn block(&self, f: &mut Frame, b: &Block) -> R<Flow> {
        for s in b {
            match self.statement(f, s)? {
                Flow::Normal => {}
                other => return Ok(other),
            }
        }
        Ok(Flow::Normal)
    }

    fn statement(&self, f: &mut Frame, s: &Statement) -> R<Flow> {
        Ok(match s {
            Statement::Emit(range) => {
                for h in range.clone() {
                    let v = self.compute(f, h)?;
                    f.vals[h.index()] = Some(v);
                }
                Flow::Normal
            }
            Statement::Block(b) => self.block(f, b)?,
            Statement::If { condition, accept, reject } => {
                if self.eval(f, *condition)?.s().b() {
                    self.block(f, accept)?
                } else {
                    self.block(f, reject)?
                }
            }
            Statement::Switch { selector, cases } => {
                let sel = self.eval(f, *selector)?.s();
                let hit = cases.iter().position(|c| match c.value {
                    SwitchValue::I32(v) => sel == S::I(v),
                    SwitchValue::U32(v) => sel == S::U(v),
                    SwitchValue::Default => false,
                });
                let start = hit.or_else(|| cases.iter().position(|c| matches!(c.value, SwitchValue::Default)));
                if let Some(mut i) = start {
                    loop {
                        match self.block(f, &cases[i].body)? {
                            Flow::Break => break,
                            Flow::Normal => {
                                if cases[i].fall_through && i + 1 < cases.len() {
                                    i += 1;
                                } else {
                                    break;
                                }
                            }
                            other => return Ok(other),
                        }
                    }
                }
                Flow::Normal
            }
            Statement::Loop { body, continuing, break_if } => {
                loop {
                    self.step()?;
                    match self.block(f, body)? {
                        Flow::Break => break,
                        Flow::Return(v) => return Ok(Flow::Return(v)),
                        Flow::Normal | Flow::Continue => {}
                    }
                    if let Flow::Return(v) = self.block(f, continuing)? {
                        return Ok(Flow::Return(v));
                    }
                    if let Some(c) = break_if {
                        if self.eval(f, *c)?.s().b() {
                            break;
                        }
                    }
                }
                Flow::Normal
            }
            Statement::Break => Flow::Break,
            Statement::Continue => Flow::Continue,
            Statement::Return { value } => Flow::Return(match value {
                Some(v) => Some(self.eval(f, *v)?),
                None => None,
            }),
            Statement::Kill => {
                self.killed.set(true);
                Flow::Normal
            }
            Statement::Store { pointer, value } => {
                let p = self.eval(f, *pointer)?;
                let v = self.eval(f, *value)?;
                match p {
                    V::Ptr(Ptr { root: Root::Local(l), path, len }) => {
                        store_path(&mut f.locals[l as usize], &path[..len as usize], v)
                    }
                    other => return Err(format!("store through {:?}", other)),
                }
                Flow::Normal
            }
            Statement::Call { function, arguments, result } => {
                let args: Vec<V> = arguments.iter().map(|a| self.eval(f, *a)).collect::<R<_>>()?;
                let func = &self.m.functions[*function];
                let (ret, _) = self.call(func, args)?;
                if let Some(r) = result {
                    f.vals[r.index()] = Some(ret.ok_or("a call without a value")?);
                }
                Flow::Normal
            }
            other => return Err(format!("unsupported statement {:?}", other)),
        })
    }

    fn call(&self, func: &Function, args: Vec<V>) -> R<(Option<V>, Vec<V>)> {
        let mut f = Frame {
            exprs: &func.expressions,
            vals: vec![None; func.expressions.len()],
            locals: Vec::with_capacity(func.local_variables.len()),
            args,
        };
        let mut inits = Vec::new();
        for (_, lv) in func.local_variables.iter() {
            f.locals.push(self.zero(lv.ty));
            inits.push(lv.init);
        }
        for (i, init) in inits.into_iter().enumerate() {
            if let Some(e) = init {
                let v = self.eval(&mut f, e)?;
                f.locals[i] = v;
            }
        }
        let ret = match self.block(&mut f, &func.body)? {
            Flow::Return(v) => v,
            _ => None,
        };
        Ok((ret, f.locals))
    }

    /// Runs the entry point `index` with these arguments over these buffers.
    pub fn run(&self, index: usize, args: Vec<V>, buffers: &Buffers) -> R<Outcome> {
        self.killed.set(false);
        self.steps.set(0);
        BUFFERS.with(|b| b.set(buffers as *const Buffers));
        let ep = &self.m.entry_points[index];
        let (result, locals) = self.call(&ep.function, args)?;
        let names = ep.function.local_variables.iter().map(|(_, l)| l.name.clone());
        Ok(Outcome { result, locals: names.zip(locals).collect() })
    }

    /// The names of the entry point's result struct members (or its one binding).
    pub fn result_fields(&self, index: usize) -> Vec<(String, Option<Binding>)> {
        let ep = &self.m.entry_points[index];
        match &ep.function.result {
            None => Vec::new(),
            Some(r) => match &self.m.types[r.ty].inner {
                TypeInner::Struct { members, .. } => members
                    .iter()
                    .map(|m| (m.name.clone().unwrap_or_default(), m.binding.clone()))
                    .collect(),
                _ => vec![(String::new(), r.binding.clone())],
            },
        }
    }
}

thread_local! {
    static BUFFERS: Cell<*const Buffers> = const { Cell::new(std::ptr::null()) };
}

fn zero_s(kind: ScalarKind) -> S {
    match kind {
        ScalarKind::Float | ScalarKind::AbstractFloat => S::F(0.0),
        ScalarKind::Sint | ScalarKind::AbstractInt => S::I(0),
        ScalarKind::Uint => S::U(0),
        ScalarKind::Bool => S::B(false),
    }
}

fn access(b: V, i: u32) -> V {
    match b {
        V::Ptr(mut p) => {
            assert!((p.len as usize) < p.path.len(), "pointer path too deep");
            p.path[p.len as usize] = i;
            p.len += 1;
            V::Ptr(p)
        }
        V::Vec(n, c) => V::S(c[(i as usize).min(n as usize - 1)]),
        V::Agg(v) => {
            let n = v.len();
            v.into_iter().nth((i as usize).min(n - 1)).unwrap()
        }
        V::Opaque => V::Opaque,
        V::S(_) => panic!("index into a scalar"),
    }
}

fn load_path(v: &V, path: &[u32]) -> V {
    if path.is_empty() {
        return v.clone();
    }
    match v {
        V::Agg(items) => load_path(&items[(path[0] as usize).min(items.len() - 1)], &path[1..]),
        V::Vec(n, c) => V::S(c[(path[0] as usize).min(*n as usize - 1)]),
        _ => panic!("load through {:?}", v),
    }
}

fn store_path(target: &mut V, path: &[u32], v: V) {
    if path.is_empty() {
        *target = v;
        return;
    }
    match target {
        V::Agg(items) => {
            let i = (path[0] as usize).min(items.len() - 1);
            store_path(&mut items[i], &path[1..], v)
        }
        V::Vec(n, c) => {
            let i = (path[0] as usize).min(*n as usize - 1);
            c[i] = v.s();
        }
        _ => panic!("store into {:?}", target),
    }
}
