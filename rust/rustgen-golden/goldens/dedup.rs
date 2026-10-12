pub mod common {
  pub mod p {
    #[derive(Clone, Copy, PartialEq, Eq, Hash, Debug)]
    #[repr(transparent)]
    pub struct Color(pub i32);

    impl Color {
      pub const UNSPECIFIED: Color = Color(0);

      /// The matchable classification of this value.
      #[inline]
      #[must_use]
      pub fn kind(self) -> ColorKind {
        match self.0 {
          0 => ColorKind::Unspecified,
          other => ColorKind::Unknown(other),
        }
      }
    }

    #[derive(Clone, Copy, PartialEq, Eq, Hash, Debug)]
    #[non_exhaustive]
    pub enum ColorKind {
      Unspecified,
      Unknown(i32),
    }

    #[derive(Clone, Copy, PartialEq, Eq, Hash, Debug)]
    #[repr(transparent)]
    pub struct ColorKind_(pub i32);

    impl ColorKind_ {
      pub const UNSPECIFIED: ColorKind_ = ColorKind_(0);

      /// The matchable classification of this value.
      #[inline]
      #[must_use]
      pub fn kind(self) -> ColorKind_Kind {
        match self.0 {
          0 => ColorKind_Kind::Unspecified,
          other => ColorKind_Kind::Unknown(other),
        }
      }
    }

    #[derive(Clone, Copy, PartialEq, Eq, Hash, Debug)]
    #[non_exhaustive]
    pub enum ColorKind_Kind {
      Unspecified,
      Unknown(i32),
    }
  }
}

pub mod arena {
  pub mod p {
    pub struct MyMsg<'a> {
      _marker: ::core::marker::PhantomData<&'a ()>,
    }

    impl<'a> MyMsg<'a> {
      pub fn inner(&self) -> Option<&'a my_msg::Inner<'a>> {
        todo!()
      }
      pub fn decode(input: &'a [u8], arena: &'a ::rapidproto::Arena<'a>) -> Result<&'a MyMsg<'a>, ::rapidproto::DecodeError> {
        todo!()
      }
    }

    pub mod my_msg {
      pub struct Inner<'a> {
        _marker: ::core::marker::PhantomData<&'a ()>,
      }

      impl<'a> Inner<'a> {
        pub fn a(&self) -> i32 {
          todo!()
        }
        pub fn decode(input: &'a [u8], arena: &'a ::rapidproto::Arena<'a>) -> Result<&'a Inner<'a>, ::rapidproto::DecodeError> {
          todo!()
        }
      }
    }

    pub struct My_Msg<'a> {
      _marker: ::core::marker::PhantomData<&'a ()>,
    }

    impl<'a> My_Msg<'a> {
      pub fn inner(&self) -> Option<&'a my_msg_::Inner<'a>> {
        todo!()
      }
      pub fn decode(input: &'a [u8], arena: &'a ::rapidproto::Arena<'a>) -> Result<&'a My_Msg<'a>, ::rapidproto::DecodeError> {
        todo!()
      }
    }

    pub mod my_msg_ {
      pub struct Inner<'a> {
        _marker: ::core::marker::PhantomData<&'a ()>,
      }

      impl<'a> Inner<'a> {
        pub fn b(&self) -> i32 {
          todo!()
        }
        pub fn decode(input: &'a [u8], arena: &'a ::rapidproto::Arena<'a>) -> Result<&'a Inner<'a>, ::rapidproto::DecodeError> {
          todo!()
        }
      }
    }

    pub struct foo_<'a> {
      _marker: ::core::marker::PhantomData<&'a ()>,
    }

    impl<'a> foo_<'a> {
      pub fn x(&self) -> i32 {
        todo!()
      }
      pub fn decode(input: &'a [u8], arena: &'a ::rapidproto::Arena<'a>) -> Result<&'a foo_<'a>, ::rapidproto::DecodeError> {
        todo!()
      }
    }

    pub struct Foo<'a> {
      _marker: ::core::marker::PhantomData<&'a ()>,
    }

    impl<'a> Foo<'a> {
      pub fn inner(&self) -> Option<&'a foo::Inner<'a>> {
        todo!()
      }
      pub fn decode(input: &'a [u8], arena: &'a ::rapidproto::Arena<'a>) -> Result<&'a Foo<'a>, ::rapidproto::DecodeError> {
        todo!()
      }
    }

    pub mod foo {
      pub struct Inner<'a> {
        _marker: ::core::marker::PhantomData<&'a ()>,
      }

      impl<'a> Inner<'a> {
        pub fn y(&self) -> i32 {
          todo!()
        }
        pub fn decode(input: &'a [u8], arena: &'a ::rapidproto::Arena<'a>) -> Result<&'a Inner<'a>, ::rapidproto::DecodeError> {
          todo!()
        }
      }
    }
  }
}
