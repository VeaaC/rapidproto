pub mod common {
  pub mod m {
    pub mod n {
      #[derive(Clone, Copy, PartialEq, Eq, Hash, Debug)]
      pub struct Color(pub i32);

      impl Color {
        pub const UNSPECIFIED: Color = Color(0);
        pub const RED: Color = Color(1);
        pub const GREEN: Color = Color(2);

        /// The matchable classification of this value.
        #[inline]
        #[must_use]
        pub fn kind(self) -> ColorKind {
          match self.0 {
            0 => ColorKind::Unspecified,
            1 => ColorKind::Red,
            2 => ColorKind::Green,
            other => ColorKind::Unknown(other),
          }
        }
      }

      #[derive(Clone, Copy, PartialEq, Eq, Hash, Debug)]
      #[non_exhaustive]
      pub enum ColorKind {
        Unspecified,
        Red,
        Green,
        Unknown(i32),
      }
    }
  }
}

pub mod arena {
  pub mod m {
    pub mod n {
      pub struct Rec<'a> {
        _marker: ::core::marker::PhantomData<&'a ()>,
      }

      impl<'a> Rec<'a> {
        pub fn key(&self) -> i64 {
          todo!()
        }
        pub fn name(&self) -> ::rapidproto::ProtoStr<'a> {
          todo!()
        }
        pub fn blob(&self) -> &'a [u8] {
          todo!()
        }
        pub fn active(&self) -> bool {
          todo!()
        }
        pub fn color(&self) -> super::super::super::common::m::n::Color {
          todo!()
        }
        pub fn maybe(&self) -> Option<i32> {
          todo!()
        }
        pub fn ratio(&self) -> f64 {
          todo!()
        }
        pub fn decode(input: &'a [u8], arena: &'a ::rapidproto::Arena<'a>) -> Result<&'a Rec<'a>, ::rapidproto::DecodeError> {
          todo!()
        }
      }
    }
  }
}
